#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#define __NEED_struct_iovec
#include <libc/bits/alltypes.h>
#include <string.h>
#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <math.h>
#include <libc/fcntl.h>
#include <time.h>

extern "C" {

// http://stackoverflow.com/questions/417142/what-is-the-maximum-length-of-a-url-in-different-browsers
#define MAX_PATHNAME_LENGTH 2000

#define INODE_TYPE uint32_t
#define INODE_FILE 1
#define INODE_DIR  2

struct inode
{
	char name[NAME_MAX+1]; // NAME_MAX actual bytes + one byte for null termination.
	inode *parent; // ID of the parent node
	inode *sibling; // ID of a sibling node (these form a singular linked list that specifies the content under a directory)
	inode *child; // ID of the first child node in a chain of children (the root of a linked list of inodes)
	uint32_t uid; // User ID of the owner
	uint32_t gid; // Group ID of the owning group
	uint32_t mode; // r/w/x modes
	time_t ctime; // Time when the inode was last modified
	time_t mtime; // Time when the content was last modified
	time_t atime; // Time when the content was last accessed
	size_t size; // Size of the file in bytes
	size_t capacity; // Amount of bytes allocated to pointer data
	uint8_t *data; // The actual file contents.

	INODE_TYPE type;

	emscripten_fetch_t *fetch;
};

#define EM_FILEDESCRIPTOR_MAGIC 0x64666d65U // 'emfd'
struct FileDescriptor
{
	uint32_t magic;
	ssize_t file_pos;
	uint32_t mode;
	uint32_t flags;

	inode *node;
};

static inode *create_inode(INODE_TYPE type)
{
	inode *i = (inode*)malloc(sizeof(inode));
	memset(i, 0, sizeof(inode));
	i->ctime = i->mtime = i->atime = time(0);
	i->type = type;
	EM_ASM(Module['print']('create_inode allocated new inode object.'));
	return i;
}

// The current working directory of the application process.
static inode *cwd_inode = 0;

static inode *filesystem_root()
{
	static inode *root_node = create_inode(INODE_DIR);
	root_node->mode = 0777;
	return root_node;
}

static inode *get_cwd()
{
	if (!cwd_inode) cwd_inode = filesystem_root();
	return cwd_inode;
}

static void set_cwd(inode *node)
{
	cwd_inode = node;
}

static void inode_abspath(inode *node, char *dst, int dstLen)
{
	if (!node)
	{
		assert(dstLen >= strlen("(null)")+1);
		strcpy(dst, "(null)");
		return;
	}
	if (node == filesystem_root())
	{
		assert(dstLen >= strlen("/")+1);
		strcpy(dst, "/");
		return;
	}
#define MAX_DIRECTORY_DEPTH 512
	inode *stack[MAX_DIRECTORY_DEPTH];
	int depth = 0;
	while(node->parent && depth < MAX_DIRECTORY_DEPTH)
	{
		stack[depth++] = node;
		node = node->parent;
	}
	char *dstEnd = dst + dstLen;
	*dstEnd-- = '\0';
	while(depth > 0 && dst < dstEnd)
	{
		if (dst < dstEnd) *dst++ = '/';
		--depth;
		int len = strlen(stack[depth]->name);
		if (len > dstEnd - dst) len = dstEnd - dst;
		strncpy(dst, stack[depth]->name, len);
		dst += len;
	}
}

static void delete_inode(inode *node)
{
	free(node);
}

// Makes node the child of parent.
static void link_inode(inode *node, inode *parent)
{
	char parentName[PATH_MAX];
	inode_abspath(parent, parentName, PATH_MAX);
	EM_ASM_INT( { Module['printErr']('link_inode: node "' + Pointer_stringify($0) + '" to parent "' + Pointer_stringify($1) + '".') }, 
		node->name, parentName);
	// When linking a node, it can't be part of the filesystem tree (but it can have children of its own)
	assert(!node->parent);
	assert(!node->sibling);

	// The inode pointed by 'node' is not yet part of the filesystem, so it's not shared memory and only this thread
	// is accessing it. Therefore setting the node's parent here is not yet racy, do that operation first.
	node->parent = parent;

	// This node is to become the first child of the parent, and the old first child of the parent should
	// become the sibling of this node, i.e.
	//  1) node->sibling = parent->child;
	//  2) parent->child = node;
	// However these two operations need to occur atomically in order to be coherent. To ensure that, run the two
	// operations in a CAS loop, which is possible because the first operation is not racy until the node is 'published'
	// to the filesystem tree by the compare_exchange operation.
	do { __atomic_load(&parent->child, &node->sibling, __ATOMIC_SEQ_CST); // node->sibling <- parent->child
	} while (!__atomic_compare_exchange(&parent->child, &node->sibling, &node, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); // parent->child <- node if it had not raced to change value in between
}

// Traverse back in sibling linked list, or 0 if no such node exist.
static inode *find_predecessor_sibling(inode *node, inode *parent)
{
	inode *child = parent->child;
	if (child == node)
		return 0;
	while(child && child->sibling != node)
		child = child->sibling;
	if (!child->sibling) return 0;
	return child;
}

static void unlink_inode(inode *node)
{
	EM_ASM_INT( { Module['printErr']('unlink_inode: node ' + Pointer_stringify($0) + ' from its parent ' + Pointer_stringify($1) + '.') }, 
		node->name, node->parent->name);
	inode *parent = node->parent;
	if (!parent) return;
	node->parent = 0;

	if (parent->child == node)
	{
		parent->child = node->sibling;
	}
	else
	{
		inode *predecessor = find_predecessor_sibling(node, parent);
		if (predecessor)
			predecessor->sibling = node->sibling;
	}
	node->parent = node->sibling = 0;
}

// Compares two strings for equality until a '\0' or a '/' is hit. Returns 0 if the strings differ,
// or a pointer to the beginning of the next directory component name of s1 if the strings are equal.
static const char *path_cmp(const char *s1, const char *s2)
{
	while(*s1 == *s2)
	{
		if (*s1 == '/') return s1+1;
		if (*s1 == '\0') return s1;
		++s1;
		++s2;
	}
	if (*s1 == '/' && *s2 == '\0') return s1+1;
	if (*s1 == '\0' && *s2 == '/') return s1;
	return 0;
}

// Copies string 'path' to 'dst', but stops on the first forward slash '/' character.
// Returns number of bytes written, excluding null terminator
static int strcpy_inodename(char *dst, const char *path)
{
	char *d = dst;
	while(*path && *path != '/')
		*dst++ = *path++;
	*dst = '\0';
	return dst - d;
}

// Returns a pointer to the basename part of the string, i.e. the string after the last occurrence of a forward slash character
static const char *basename_part(const char *path)
{
	const char *s = path;
	while(*path)
	{
		if (*path == '/') s = path+1;
		++path;
	}
	return s;
}

static inode *create_directory_hierarchy_for_file(inode *root, const char *path_to_file, unsigned int mode)
{
	assert(root);
	if (!root) return 0;

	inode *node = root->child;
	while(node)
	{
		const char *child_path = path_cmp(path_to_file, node->name);
		EM_ASM_INT( { Module['printErr']('path_cmp ' + Pointer_stringify($0) + ', ' + Pointer_stringify($1) + ', ' + Pointer_stringify($2) + ' .') }, path_to_file, node->name, child_path);
		if (child_path)
		{
			// The directory name matches.
			path_to_file = child_path;
			if (path_to_file[0] == '\0') return node;
			if (path_to_file[0] == '/' && path_to_file[1] == '\0' /* && node is a directory*/) return node;
			root = node;
			node = node->child;
		}
		else
		{
			node = node->sibling;
		}
	}
	EM_ASM_INT( { Module['printErr']('path_to_file ' + Pointer_stringify($0) + ' .') }, path_to_file);
	const char *basename_pos = basename_part(path_to_file);
	EM_ASM_INT( { Module['printErr']('basename_pos ' + Pointer_stringify($0) + ' .') }, basename_pos);
	while(*path_to_file && path_to_file < basename_pos)
	{
		node = create_inode(INODE_DIR);
		node->mode = mode;
		path_to_file += strcpy_inodename(node->name, path_to_file) + 1;
		link_inode(node, root);
		EM_ASM_INT( { Module['print']('create_directory_hierarchy_for_file: created directory ' + Pointer_stringify($0) + ' under parent ' + Pointer_stringify($1) + '.') }, 
			node->name, node->parent->name);
		root = node;
	}
	return root;
}
// Same as above, but the root node is deduced from 'path'. (either absolute if path starts with "/", or relative)
static inode *create_directory_hierarchy_for_file(const char *path, unsigned int mode)
{
	inode *root;
	if (path[0] == '/') root = filesystem_root(), ++path;
	else root = get_cwd();
	return create_directory_hierarchy_for_file(root, path, mode);
}

// Given a path to a file, finds the inode of the parent directory that contains the file, or 0 if the intermediate path doesn't exist.
static inode *find_parent_inode(inode *root, const char *path)
{
	EM_ASM_INT( { Module['print']('find_parent_inode: inode: ' + Pointer_stringify($0) + ' path: ' + Pointer_stringify($1) + '.') }, 
		root ? root->name : "(null)", path);
	if (!root) return 0;
	const char *basename = basename_part(path);
	inode *node = root->child;
	while(node)
	{
		const char *child_path = path_cmp(path, node->name);
		if (child_path)
		{
			// The directory name matches.
			path = child_path;
			if (path >= basename) return node;
			if (!*path) return 0;
			node = node->child;
		}
		else
		{
			node = node->sibling;
		}
	}
	return root;
}

// Given a root inode of the filesystem and a path relative to it, e.g. "some/directory/dir_or_file",
// returns the inode that corresponds to "dir_or_file", or 0 if it doesn't exist.
// If the parameter out_closest_parent is specified, the closest (grand)parent node will be returned.
static inode *find_inode(inode *root, const char *path, inode **out_closest_parent = 0)
{
	if (out_closest_parent) *out_closest_parent = root;
	if (!root)
	{
		return 0;
	}

	if (path[0] == 0
	|| (path[0] == '/' && path[1] == '\0'))
		return root; // special-case finding empty string path "" or "/" returns the root searched in.

	inode *node = root->child;
	while(node)
	{
		const char *child_path = path_cmp(path, node->name);
		if (child_path)
		{
			// The directory name matches.
			path = child_path;
			if (path[0] == '\0') return node;
			if (path[0] == '/' && path[1] == '\0' /* && node is a directory*/) return node;
			if (out_closest_parent) *out_closest_parent = node;
			node = node->child;
		}
		else
		{
			node = node->sibling;
		}
	}
	return 0;
}

// Same as above, but the root node is deduced from 'path'. (either absolute if path starts with "/", or relative)
static inode *find_inode(const char *path, inode **out_closest_parent = 0)
{
	inode *root;
	if (path[0] == '/') root = filesystem_root(), ++path;
	else root = get_cwd();
	return find_inode(root, path, out_closest_parent);
}

// Debug function that dumps out the filesystem tree to console.
void emscripten_dump_fs_tree(inode *root, char *path)
{
	printf("%s:\n", path);
	// Print out:
	// file mode | number of links | owner name | group name | file size in bytes | file last modified time | path name
	// which aligns with "ls -AFTRl" on console
	inode *child = root->child;
	uint64_t totalSize = 0;
	while(child)
	{
		printf("%c%c%c%c%c%c%c%c%c%c  %d user%u group%u %u Jan 1 1970 %s%c\n",
			child->type == INODE_DIR ? 'd' : '-',
			(child->mode & S_IRUSR) ? 'r' : '-',
			(child->mode & S_IWUSR) ? 'w' : '-',
			(child->mode & S_IXUSR) ? 'x' : '-',
			(child->mode & S_IRGRP) ? 'r' : '-',
			(child->mode & S_IWGRP) ? 'w' : '-',
			(child->mode & S_IXGRP) ? 'x' : '-',
			(child->mode & S_IROTH) ? 'r' : '-',
			(child->mode & S_IWOTH) ? 'w' : '-',
			(child->mode & S_IXOTH) ? 'x' : '-',
			1, // number of links to this file
			child->uid,
			child->gid,
			child->size,
			child->name,
			child->type == INODE_DIR ? '/' : ' ');
		totalSize += child->size;
		child = child->sibling;
	}
	printf("total %llu bytes\n\n", totalSize);

	child = root->child;
	char *path_end = path + strlen(path);
	while(child)
	{
		if (child->type == INODE_DIR)
		{
			strcpy(path_end, child->name);
			strcat(path_end, "/");
			emscripten_dump_fs_tree(child, path);
		}
		child = child->sibling;
	}
}

void emscripten_dump_fs_root()
{
	char path[PATH_MAX] = "/";
	emscripten_dump_fs_tree(filesystem_root(), path);
}

#define RETURN_ERRNO(errno, error_reason) do { \
		EM_ASM_INT({ Module['printErr'](Pointer_stringify($0) + '() returned errno ' + #errno + ': ' + error_reason + '!')}, __FUNCTION__); \
		return -errno; \
	} while(0)

// http://man7.org/linux/man-pages/man2/open.2.html
long __syscall5(int which, ...) // open
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char*);
	int flags = va_arg(vl, int);
	int mode = va_arg(vl, int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('open(pathname="' + Pointer_stringify($0) + '", flags=0x' + ($1).toString(16) + ', mode=0' + ($2).toString(8) + ')') },
		pathname, flags, mode);

	int accessMode = (flags & O_ACCMODE);

	if ((flags & O_ASYNC)) RETURN_ERRNO(ENOTSUP, "TODO: Opening files with O_ASYNC flag is not supported in ASMFS");
	if ((flags & O_DIRECT)) RETURN_ERRNO(ENOTSUP, "TODO: O_DIRECT flag is not supported in ASMFS");
	if ((flags & O_DSYNC)) RETURN_ERRNO(ENOTSUP, "TODO: O_DSYNC flag is not supported in ASMFS");
	if ((flags & O_EXCL) && !(flags & O_CREAT)) RETURN_ERRNO(EINVAL, "open() with O_EXCL flag needs to always be paired with O_CREAT"); // Spec says the behavior is undefined, we can just enforce it
	if ((flags & (O_NONBLOCK|O_NDELAY))) RETURN_ERRNO(ENOTSUP, "TODO: Opening files with O_NONBLOCK or O_NDELAY flags is not supported in ASMFS");
	if ((flags & O_PATH)) RETURN_ERRNO(ENOTSUP, "TODO: Opening files with O_PATH flag is not supported in ASMFS");
	if ((flags & O_SYNC)) RETURN_ERRNO(ENOTSUP, "TODO: Opening files with O_SYNC flag is not supported in ASMFS");

	// The flags:O_CLOEXEC flag is ignored, doesn't have meaning for Emscripten

	// TODO: the flags:O_DIRECT flag seems like a great way to let applications explicitly control XHR/IndexedDB read/write buffering behavior?

	// The flags:O_LARGEFILE flag is ignored, we should always be largefile-compatible

	// TODO: The flags:O_NOATIME is ignored, file access times have not been implemented yet
	// The flags O_NOCTTY, O_NOFOLLOW

	if ((flags & O_TMPFILE))
	{
		if (accessMode != O_WRONLY && accessMode != O_RDWR) RETURN_ERRNO(EINVAL, "O_TMPFILE was specified in flags, but neither O_WRONLY nor O_RDWR was specified");
		else RETURN_ERRNO(EOPNOTSUPP, "TODO: The filesystem containing pathname does not support O_TMPFILE");
	}

	// TODO: if (too_many_files_open) RETURN_ERRNO(EMFILE, "The per-process limit on the number of open file descriptors has been reached, see getrlimit(RLIMIT_NOFILE)");

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	// Find if this file exists already in the filesystem?
	inode *root = (pathname[0] == '/') ? filesystem_root() : get_cwd();
	const char *relpath = (pathname[0] == '/') ? pathname+1 : pathname;

	inode *node = find_inode(root, relpath);
	if (node)
	{
		if ((flags & O_DIRECTORY) && node->type != INODE_DIR) RETURN_ERRNO(ENOTDIR, "O_DIRECTORY was specified and pathname was not a directory");
		if (!(node->mode & 0444)) RETURN_ERRNO(EACCES, "The requested access to the file is not allowed");
		if ((flags & O_CREAT) && (flags & O_EXCL)) RETURN_ERRNO(EEXIST, "pathname already exists and O_CREAT and O_EXCL were used");
		if (node->type == INODE_DIR && accessMode != O_RDONLY) RETURN_ERRNO(EISDIR, "pathname refers to a directory and the access requested involved writing (that is, O_WRONLY or O_RDWR is set)");
	}

	if (node && node->fetch) emscripten_fetch_wait(node->fetch, INFINITY);

	if ((flags & O_CREAT) || (flags & O_TRUNC) || (flags & O_EXCL))
	{
		// Create a new empty file or truncate existing one.
		if (node)
		{
			if (node->fetch) emscripten_fetch_close(node->fetch);
			node->fetch = 0;
			node->size = 0;
		}
		else
		{
			inode *directory = create_directory_hierarchy_for_file(root, relpath, mode);
			node = create_inode((flags & O_DIRECTORY) ? INODE_DIR : INODE_FILE);
			node->mode = mode;
			strcpy(node->name, basename_part(pathname));
			link_inode(node, directory);
		}
	}
	else if (!node || (!node->fetch && !node->data))
	{
		emscripten_fetch_t *fetch = 0;
		if (!(flags & O_DIRECTORY) && accessMode != O_WRONLY)
		{
			// If not, we'll need to fetch it.
			emscripten_fetch_attr_t attr;
			emscripten_fetch_attr_init(&attr);
			strcpy(attr.requestMethod, "GET");
			attr.attributes = EMSCRIPTEN_FETCH_APPEND | EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_WAITABLE | EMSCRIPTEN_FETCH_PERSIST_FILE;
			fetch = emscripten_fetch(&attr, pathname);

		// switch(fopen_mode)
		// {
		// case synchronous_fopen:
			emscripten_fetch_wait(fetch, INFINITY);

			if (fetch->status != 200 || fetch->totalBytes == 0)
			{
				emscripten_fetch_close(fetch);
				RETURN_ERRNO(ENOENT, "O_CREAT is not set and the named file does not exist (attempted emscripten_fetch() XHR to download)");
			}
		//  break;
		// case asynchronous_fopen:
		//  break;
		// }
		}

		if (node)
		{
			// If we had an existing inode entry, just associate the entry with the newly fetched data.
			if (node->type == INODE_FILE) node->fetch = fetch;
		}
		else if ((flags & O_CREAT) // If the filesystem entry did not exist, but we have a create flag, ...
			|| (!node && fetch)) // ... or if it did not exist in our fs, but it could be found via fetch(), ...
		{
			// ... add it as a new entry to the fs.
			inode *directory = create_directory_hierarchy_for_file(root, relpath, mode);
			node = create_inode((flags & O_DIRECTORY) ? INODE_DIR : INODE_FILE);
			node->mode = mode;
			strcpy(node->name, basename_part(pathname));
			node->fetch = fetch;
			link_inode(node, directory);
		}
		else
		{
			if (fetch) emscripten_fetch_close(fetch);
			RETURN_ERRNO(ENOENT, "O_CREAT is not set and the named file does not exist");
		}
		node->size = node->fetch->totalBytes;
		emscripten_dump_fs_root();
	}

	FileDescriptor *desc = (FileDescriptor*)malloc(sizeof(FileDescriptor));
	desc->magic = EM_FILEDESCRIPTOR_MAGIC;
	desc->node = node;
	desc->file_pos = ((flags & O_APPEND) && node->fetch) ? node->fetch->totalBytes : 0;
	desc->mode = mode;
	desc->flags = flags;

	// TODO: The file descriptor needs to be a small number, man page:
	// "a small, nonnegative integer for use in subsequent system calls
	// (read(2), write(2), lseek(2), fcntl(2), etc.).  The file descriptor
	// returned by a successful call will be the lowest-numbered file
	// descriptor not currently open for the process."
	return (long)desc;
}

// http://man7.org/linux/man-pages/man2/close.2.html
long __syscall6(int which, ...) // close
{
	va_list vl;
	va_start(vl, which);
	int fd = va_arg(vl, int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('close(fd=' + $0 + ')') }, fd);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "fd isn't a valid open file descriptor");

	if (desc->node && desc->node->fetch)
	{
		emscripten_fetch_wait(desc->node->fetch, INFINITY); // TODO: This should not be necessary- test this out
		emscripten_fetch_close(desc->node->fetch);
		desc->node->fetch = 0;
	}
	desc->magic = 0;
	free(desc);
	return 0;
}

// http://man7.org/linux/man-pages/man2/sysctl.2.html
long __syscall54(int which, ...) // sysctl
{
	EM_ASM( { Module['printErr']('sysctl() is ignored') });
	return 0;
}

// http://man7.org/linux/man-pages/man2/llseek.2.html
// also useful: http://man7.org/linux/man-pages/man2/lseek.2.html
long __syscall140(int which, ...) // llseek
{
	va_list vl;
	va_start(vl, which);
	unsigned int fd = va_arg(vl, unsigned int);
	unsigned long offset_high = va_arg(vl, unsigned long);
	unsigned long offset_low = va_arg(vl, unsigned long);
	off_t *result = va_arg(vl, off_t *);
	unsigned int whence = va_arg(vl, unsigned int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('llseek(fd=' + $0 + ', offset=0x' + (($1<<32)|$2) + ', result=0x' + ($3).toString(16) + ', whence=' + $4 + ')') },
		fd, offset_high, offset_low, result, whence);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "fd isn't a valid open file descriptor");

	if (desc->node->fetch) emscripten_fetch_wait(desc->node->fetch, INFINITY);

	int64_t offset = (int64_t)(((uint64_t)offset_high << 32) | (uint64_t)offset_low);
	int64_t newPos;
	switch(whence)
	{
		case SEEK_SET: newPos = offset; break;
		case SEEK_CUR: newPos = desc->file_pos + offset; break;
		case SEEK_END: newPos = (desc->node->fetch ? desc->node->fetch->numBytes : desc->node->size) + offset; break;
		case 3/*SEEK_DATA*/: RETURN_ERRNO(EINVAL, "whence is invalid (sparse files, whence=SEEK_DATA, is not supported");
		case 4/*SEEK_HOLE*/: RETURN_ERRNO(EINVAL, "whence is invalid (sparse files, whence=SEEK_HOLE, is not supported");
		default: RETURN_ERRNO(EINVAL, "whence is invalid");
	}
	if (newPos < 0) RETURN_ERRNO(EINVAL, "The resulting file offset would be negative");
	if (newPos > 0x7FFFFFFFLL) RETURN_ERRNO(EOVERFLOW, "The resulting file offset cannot be represented in an off_t");

	desc->file_pos = newPos;

	if (result) *result = desc->file_pos;
	return 0;
}

// http://man7.org/linux/man-pages/man2/readv.2.html
long __syscall145(int which, ...) // readv
{
	va_list vl;
	va_start(vl, which);
	int fd = va_arg(vl, int);
	const iovec *iov = va_arg(vl, const iovec*);
	int iovcnt = va_arg(vl, int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('readv(fd=' + $0 + ', iov=0x' + ($1).toString(16) + ', iovcnt=' + $2 + ')') }, fd, iov, iovcnt);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "fd isn't a valid open file descriptor");

	inode *node = desc->node;
	if (!node) RETURN_ERRNO(-1, "ASMFS internal error: file descriptor points to a non-file");
	if (node->type == INODE_DIR) RETURN_ERRNO(EISDIR, "fd refers to a directory");
	if (node->type != INODE_FILE /* TODO: && node->type != socket */) RETURN_ERRNO(EINVAL, "fd is attached to an object which is unsuitable for reading");

	// TODO: if (node->type == INODE_FILE && desc has O_NONBLOCK && read would block) RETURN_ERRNO(EAGAIN, "The file descriptor fd refers to a file other than a socket and has been marked nonblocking (O_NONBLOCK), and the read would block");
	// TODO: if (node->type == socket && desc has O_NONBLOCK && read would block) RETURN_ERRNO(EWOULDBLOCK, "The file descriptor fd refers to a socket and has been marked nonblocking (O_NONBLOCK), and the read would block");

	if (node->fetch) emscripten_fetch_wait(node->fetch, INFINITY);

	if (node->size > 0 && !node->data && (!node->fetch || !node->fetch->data)) RETURN_ERRNO(-1, "ASMFS internal error: no file data available");
	if (iovcnt < 0) RETURN_ERRNO(EINVAL, "The vector count, iovcnt, is less than zero");

	ssize_t total_read_amount = 0;
	for(int i = 0; i < iovcnt; ++i)
	{
		ssize_t n = total_read_amount + iov[i].iov_len;
		if (n < total_read_amount) RETURN_ERRNO(EINVAL, "The sum of the iov_len values overflows an ssize_t value");
		if (!iov[i].iov_base && iov[i].iov_len > 0) RETURN_ERRNO(EINVAL, "iov_len specifies a positive length buffer but iov_base is a null pointer");
		total_read_amount = n;
	}

	size_t offset = desc->file_pos;
	uint8_t *data = node->data ? node->data : (node->fetch ? (uint8_t *)node->fetch->data : 0);
	for(int i = 0; i < iovcnt; ++i)
	{
		ssize_t dataLeft = node->size - offset;
		if (dataLeft <= 0) break;
		size_t bytesToCopy = (size_t)dataLeft < iov[i].iov_len ? dataLeft : iov[i].iov_len;
		memcpy(iov[i].iov_base, &data[offset], bytesToCopy);
		offset += bytesToCopy;
	}
	ssize_t numRead = offset - desc->file_pos;
	desc->file_pos = offset;
	return numRead;
}

static char stdout_buffer[4096] = {};
static int stdout_buffer_end = 0;
static char stderr_buffer[4096] = {};
static int stderr_buffer_end = 0;

static void print_stream(void *bytes, int numBytes, bool stdout)
{
	char *buffer = stdout ? stdout_buffer : stderr_buffer;
	int &buffer_end = stdout ? stdout_buffer_end : stderr_buffer_end;

	memcpy(buffer + buffer_end, bytes, numBytes);
	buffer_end += numBytes;
	int new_buffer_start = 0;
	for(int i = 0; i < buffer_end; ++i)
	{
		if (buffer[i] == '\n')
		{
			buffer[i] = 0;
			EM_ASM_INT( { Module['print'](Pointer_stringify($0)) }, buffer+new_buffer_start);
			new_buffer_start = i+1;
		}
	}
	size_t new_buffer_size = buffer_end - new_buffer_start;
	memmove(buffer, buffer + new_buffer_start, new_buffer_size);
	buffer_end = new_buffer_size;
}

// http://man7.org/linux/man-pages/man2/writev.2.html
long __syscall146(int which, ...) // writev
{
	va_list vl;
	va_start(vl, which);
	int fd = va_arg(vl, int);
	const iovec *iov = va_arg(vl, const iovec*);
	int iovcnt = va_arg(vl, int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('writev(fd=' + $0 + ', iov=0x' + ($1).toString(16) + ', iovcnt=' + $2 + ')') }, fd, iov, iovcnt);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (fd != 1/*stdout*/ && fd != 2/*stderr*/) // TODO: Resolve the hardcoding of stdin,stdout & stderr
	{
		if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "fd isn't a valid open file descriptor");
	}

	if (iovcnt < 0) RETURN_ERRNO(EINVAL, "The vector count, iovcnt, is less than zero");

	ssize_t total_write_amount = 0;
	for(int i = 0; i < iovcnt; ++i)
	{
		ssize_t n = total_write_amount + iov[i].iov_len;
		if (n < total_write_amount) RETURN_ERRNO(EINVAL, "The sum of the iov_len values overflows an ssize_t value");
		if (!iov[i].iov_base && iov[i].iov_len > 0) RETURN_ERRNO(EINVAL, "iov_len specifies a positive length buffer but iov_base is a null pointer");
		total_write_amount = n;
	}

	if (fd == 1/*stdout*/ || fd == 2/*stderr*/)
	{
		ssize_t bytesWritten = 0;
		for(int i = 0; i < iovcnt; ++i)
		{
			print_stream(iov[i].iov_base, iov[i].iov_len, fd == 1);
			bytesWritten += iov[i].iov_len;
		}
		return bytesWritten;
	}
	else
	{
		// Enlarge the file in memory to fit space for the new data
		size_t newSize = desc->file_pos + total_write_amount;
		inode *node = desc->node;
		if (node->capacity < newSize)
		{
			size_t newCapacity = (newSize > (size_t)(node->capacity*1.25) ? newSize : (size_t)(node->capacity*1.25)); // Geometric increases in size for amortized O(1) behavior
			uint8_t *newData = (uint8_t *)realloc(node->data, newCapacity);
			if (!newData)
			{
				newData = (uint8_t *)malloc(newCapacity);
				memcpy(newData, node->data, node->size);
				// TODO: init gaps with zeroes.
				free(node->data);
			}
			node->data = newData;
			node->size = newSize;
			node->capacity = newCapacity;
		}

		for(int i = 0; i < iovcnt; ++i)
		{
			memcpy((uint8_t*)node->data + desc->file_pos, iov[i].iov_base, iov[i].iov_len);
			desc->file_pos += iov[i].iov_len;
		}
	}
	return total_write_amount;
}

// http://man7.org/linux/man-pages/man2/write.2.html
long __syscall4(int which, ...) // write
{
	va_list vl;
	va_start(vl, which);
	int fd = va_arg(vl, int);
	void *buf = va_arg(vl, void *);
	size_t count = va_arg(vl, size_t);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('write(fd=' + $0 + ', buf=0x' + ($1).toString(16) + ', count=' + $2 + ')') }, fd, buf, count);

	iovec io = { buf, count };
	return __syscall146(146, fd, &io, 1);
}

// http://man7.org/linux/man-pages/man2/chdir.2.html
long __syscall12(int which, ...) // chdir
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char *);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('chdir(pathname="' + Pointer_stringify($0) + '")') }, pathname);

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	inode *node = find_inode(pathname);

	// TODO: if (no permissions to navigate the tree to the path) RETURN_ERRNO(EACCES, "Search permission is denied for one of the components of path");
	// TODO: if (too many symlinks) RETURN_ERRNO(ELOOP, "Too many symbolic links were encountered in resolving path");

	// TODO: Ensure that this is checked for all components of the path
	if (!node) RETURN_ERRNO(ENOENT, "The directory specified in path does not exist");

	// TODO: Ensure that this is checked for all components of the path
	if (node->type != INODE_DIR) RETURN_ERRNO(ENOTDIR, "A component of path is not a directory");

	set_cwd(node);
	return 0;
}

// http://man7.org/linux/man-pages/man2/chmod.2.html
long __syscall15(int which, ...) // chmod
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char *);
	int mode = va_arg(vl, int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('chmod(pathname="' + Pointer_stringify($0) + '", mode=0' + ($1).toString(8) + ')') }, pathname, mode);

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	inode *node = find_inode(pathname);

	// TODO: if (no permissions to navigate the tree to the path) RETURN_ERRNO(EACCES, "Search permission is denied on a component of the path prefix");
	// TODO: if (too many symlinks) RETURN_ERRNO(ELOOP, "Too many symbolic links were encountered in resolving pathname");

	// TODO: Ensure that this is checked for all components of the path
	if (!node) RETURN_ERRNO(ENOENT, "The file does not exist");

	// TODO: Ensure that this is checked for all components of the path
	if (node->type != INODE_DIR) RETURN_ERRNO(ENOTDIR, "A component of the path prefix is not a directory");

	// TODO: if (not allowed) RETURN_ERRNO(EPERM, "The effective UID does not match the owner of the file");
	// TODO: read-only filesystems: if (fs is read-only) RETURN_ERRNO(EROFS, "The named file resides on a read-only filesystem");

	node->mode = mode;
	return 0;
}

// http://man7.org/linux/man-pages/man2/mkdir.2.html
long __syscall39(int which, ...) // mkdir
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char *);
	mode_t mode = va_arg(vl, mode_t);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('mkdir(pathname="' + Pointer_stringify($0) + '", mode=0' + ($1).toString(8) + ')') }, pathname, mode);

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	inode *root = (pathname[0] == '/') ? filesystem_root() : get_cwd();
	const char *relpath = (pathname[0] == '/') ? pathname+1 : pathname;
	inode *parent_dir = find_parent_inode(root, relpath);

	if (!parent_dir) RETURN_ERRNO(ENOENT, "A directory component in pathname does not exist or is a dangling symbolic link");

	// TODO: if (component of path wasn't actually a directory) RETURN_ERRNO(ENOTDIR, "A component used as a directory in pathname is not, in fact, a directory");

	inode *existing = find_inode(parent_dir, basename_part(pathname));
	if (existing) RETURN_ERRNO(EEXIST, "pathname already exists (not necessarily as a directory)");
	if (!(parent_dir->mode & 0222)) RETURN_ERRNO(EACCES, "The parent directory does not allow write permission to the process");

	// TODO: if (too many symlinks when traversing path) RETURN_ERRNO(ELOOP, "Too many symbolic links were encountered in resolving pathname");
	// TODO: if (any parent dir in path doesn't have search permissions) RETURN_ERRNO(EACCES, "One of the directories in pathname did not allow search permission");
	// TODO: read-only filesystems: if (fs is read-only) RETURN_ERRNO(EROFS, "Pathname refers to a file on a read-only filesystem");

	inode *directory = create_inode(INODE_DIR);
	strcpy(directory->name, basename_part(pathname));
	directory->mode = mode;
	link_inode(directory, parent_dir);
	return 0;
}

// http://man7.org/linux/man-pages/man2/rmdir.2.html
long __syscall40(int which, ...) // rmdir
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char *);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('rmdir(pathname="' + Pointer_stringify($0) + '")') }, pathname);

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	if (!strcmp(pathname, ".") || (len >= 2 && !strcmp(pathname+len-2, "/."))) RETURN_ERRNO(EINVAL, "pathname has . as last component");
	if (!strcmp(pathname, "..") || (len >= 3 && !strcmp(pathname+len-3, "/.."))) RETURN_ERRNO(ENOTEMPTY, "pathname has .. as its final component");

	inode *node = find_inode(pathname);
	if (!node) RETURN_ERRNO(ENOENT, "directory does not exist");

	// TODO: RETURN_ERRNO(ENOENT, "A directory component in pathname does not exist or is a dangling symbolic link");
	// TODO: RETURN_ERRNO(ELOOP, "Too many symbolic links were encountered in resolving pathname");
	// TODO: RETURN_ERRNO(EACCES, "one of the directories in the path prefix of pathname did not allow search permission");

	if (node == filesystem_root() || node == get_cwd()) RETURN_ERRNO(EBUSY, "pathname is currently in use by the system or some process that prevents its removal (pathname is currently used as a mount point or is the root directory of the calling process)");
	if (node->parent && !(node->parent->mode & 0222)) RETURN_ERRNO(EACCES, "Write access to the directory containing pathname was not allowed");
	if (node->type != INODE_DIR) RETURN_ERRNO(ENOTDIR, "pathname is not a directory");
	if (node->child) RETURN_ERRNO(ENOTEMPTY, "pathname contains entries other than . and ..");

	// TODO: RETURN_ERRNO(EPERM, "The directory containing pathname has the sticky bit (S_ISVTX) set and the process's effective user ID is neither the user ID of the file to be deleted nor that of the directory containing it, and the process is not privileged");
	// TODO: RETURN_ERRNO(EROFS, "pathname refers to a directory on a read-only filesystem");

	unlink_inode(node);

	return 0;
}

// http://man7.org/linux/man-pages/man2/unlink.2.html
long __syscall10(int which, ...) // unlink
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char *);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('unlink(pathname="' + Pointer_stringify($0) + '")') }, pathname);

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	inode *node = find_inode(pathname);
	if (!node) RETURN_ERRNO(ENOENT, "file does not exist");

	inode *parent = node->parent;

	// TODO: RETURN_ERRNO(ENOENT, "A component in pathname does not exist or is a dangling symbolic link");
	// TODO: RETURN_ERRNO(ELOOP, "Too many symbolic links were encountered in translating pathname");
	// TODO: RETURN_ERRNO(EACCES, "one of the directories in the path prefix of pathname did not allow search permission");

	if (parent && !(parent->mode & 0222))
		RETURN_ERRNO(EACCES, "Write access to the directory containing pathname is not allowed for the process's effective UID");

	// TODO: RETURN_ERRNO(ENOTDIR, "A component used as a directory in pathname is not, in fact, a directory");
	// TODO: RETURN_ERRNO(EPERM, "The directory containing pathname has the sticky bit (S_ISVTX) set and the process's effective user ID is neither the user ID of the file to be deleted nor that of the directory containing it, and the process is not privileged");
	// TODO: RETURN_ERRNO(EROFS, "pathname refers to a file on a read-only filesystem");

	if (!(node->mode & 0222))
	{
		if (node->type == INODE_DIR) RETURN_ERRNO(EISDIR, "directory deletion not permitted"); // Linux quirk: Return EISDIR error for not having permission to delete a directory.
		else RETURN_ERRNO(EPERM, "file deletion not permitted"); // but return EPERM error for no permission to delete a file.
	}

	if (node->child) RETURN_ERRNO(EISDIR, "directory is not empty"); // Linux quirk: Return EISDIR error if not being able to delete a nonempty directory.

	unlink_inode(node);

	return 0;
}

// http://man7.org/linux/man-pages/man2/faccessat.2.html
long __syscall33(int which, ...) // access
{
	va_list vl;
	va_start(vl, which);
	const char *pathname = va_arg(vl, const char *);
	int mode = va_arg(vl, int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('access(pathname="' + Pointer_stringify($0) + '", mode=0' + ($1).toString(8) + ')') }, pathname, mode);

	int len = strlen(pathname);
	if (len > MAX_PATHNAME_LENGTH) RETURN_ERRNO(ENAMETOOLONG, "pathname was too long");
	if (len == 0) RETURN_ERRNO(ENOENT, "pathname is empty");

	if ((mode & F_OK) && (mode & (R_OK | W_OK | X_OK))) RETURN_ERRNO(EINVAL, "mode was incorrectly specified");

	inode *node = find_inode(pathname);
	if (!node) RETURN_ERRNO(ENOENT, "A component of pathname does not exist or is a dangling symbolic link");

	// TODO: RETURN_ERRNO(ENOENT, "A component of pathname does not exist or is a dangling symbolic link");

	// Just testing if a file exists?
	if ((mode & F_OK)) return 0;

	// TODO: RETURN_ERRNO(ELOOP, "Too many symbolic links were encountered in resolving pathname");
	// TODO: RETURN_ERRNO(EACCES, "search permission is denied for one of the directories in the path prefix of pathname");
	// TODO: RETURN_ERRNO(ENOTDIR, "A component used as a directory in pathname is not, in fact, a directory");
	// TODO: RETURN_ERRNO(EROFS, "Write permission was requested for a file on a read-only filesystem");

	if ((mode & R_OK) && !(node->mode & 0444)) RETURN_ERRNO(EACCES, "Read access would be denied to the file");
	if ((mode & W_OK) && !(node->mode & 0222)) RETURN_ERRNO(EACCES, "Write access would be denied to the file");
	if ((mode & X_OK) && !(node->mode & 0111)) RETURN_ERRNO(EACCES, "Execute access would be denied to the file");

	return 0;
}

// http://man7.org/linux/man-pages/man2/getdents.2.html
long __syscall220(int which, ...) // getdents64 (get directory entries 64-bit)
{
	va_list vl;
	va_start(vl, which);
	unsigned int fd = va_arg(vl, unsigned int);
	dirent *de = va_arg(vl, dirent*);
	unsigned int count = va_arg(vl, unsigned int);
	va_end(vl);
	unsigned int dirents_size = count / sizeof(de); // The number of dirent structures that can fit into the provided buffer.
	dirent *de_end = de + dirents_size;
	EM_ASM_INT({ Module['printErr']('getdents64(fd=' + $0 + ', de=0x' + ($1).toString(16) + ', count=' + $2 + ')') }, fd, de, count);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "Invalid file descriptor fd");

	inode *node = desc->node;
	if (!node) RETURN_ERRNO(ENOENT, "No such directory");
	if (dirents_size == 0) RETURN_ERRNO(EINVAL, "Result buffer is too small");
	if (node->type != INODE_DIR) RETURN_ERRNO(ENOTDIR, "File descriptor does not refer to a directory");

	inode *dotdot = node->parent ? node->parent : node; // In "/", the directory ".." refers to itself.

	ssize_t orig_file_pos = desc->file_pos;
	ssize_t file_pos = 0;
	// There are always two hardcoded directories "." and ".."
	if (de >= de_end) return desc->file_pos - orig_file_pos;
	if (desc->file_pos <= file_pos)
	{
		de->d_ino = (ino_t)node; // TODO: Create inode numbers instead of using pointers
		de->d_off = file_pos;
		de->d_reclen = sizeof(dirent);
		de->d_type = DT_DIR;
		strcpy(de->d_name, ".");
		++de;
		desc->file_pos += sizeof(dirent);
	}
	file_pos += sizeof(dirent);

	if (de >= de_end) return desc->file_pos - orig_file_pos;
	if (desc->file_pos <= file_pos)
	{
		de->d_ino = (ino_t)dotdot; // TODO: Create inode numbers instead of using pointers
		de->d_off = file_pos;
		de->d_reclen = sizeof(dirent);
		de->d_type = DT_DIR;
		strcpy(de->d_name, "..");
		++de;
		desc->file_pos += sizeof(dirent);
	}
	file_pos += sizeof(dirent);

	node = node->child;
	while(node && de < de_end)
	{
		if (desc->file_pos <= file_pos)
		{
			de->d_ino = (ino_t)node; // TODO: Create inode numbers instead of using pointers
			de->d_off = file_pos;
			de->d_reclen = sizeof(dirent);
			de->d_type = (node->type == INODE_DIR) ? DT_DIR : DT_REG /*Regular file*/;
			de->d_name[255] = 0;
			strncpy(de->d_name, node->name, 255);
			++de;
			desc->file_pos += sizeof(dirent);
		}
		node = node->sibling;
		file_pos += sizeof(dirent);
	}

	return desc->file_pos - orig_file_pos;
}

// http://man7.org/linux/man-pages/man2/fsync.2.html
long __syscall118(int which, ...) // fsync
{
	va_list vl;
	va_start(vl, which);
	unsigned int fd = va_arg(vl, unsigned int);
	va_end(vl);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "fd isn't a valid open file descriptor");

	inode *node = desc->node;
	if (!node)
	{
		assert(false); // TODO: Internal error handling?
		return -1;
	}

	return 0;
}

// http://man7.org/linux/man-pages/man2/dup.2.html
long __syscall41(int which, ...) // dup
{
	va_list vl;
	va_start(vl, which);
	unsigned int fd = va_arg(vl, unsigned int);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('dup(fd=' + $0 + ')') }, fd);

	FileDescriptor *desc = (FileDescriptor*)fd;
	if (!desc || desc->magic != EM_FILEDESCRIPTOR_MAGIC) RETURN_ERRNO(EBADF, "fd isn't a valid open file descriptor");

	inode *node = desc->node;
	if (!node) RETURN_ERRNO(-1, "ASMFS internal error: file descriptor points to a nonexisting file");

	// TODO: RETURN_ERRNO(EMFILE, "The per-process limit on the number of open file descriptors has been reached (see RLIMIT_NOFILE)");

	EM_ASM({ Module['printErr']('TODO: dup() is a stub and not yet implemented') });
	return 0;
}

// http://man7.org/linux/man-pages/man2/getcwd.2.html
long __syscall183(int which, ...) // getcwd
{
	va_list vl;
	va_start(vl, which);
	char *buf = va_arg(vl, char *);
	size_t size = va_arg(vl, size_t);
	va_end(vl);
	EM_ASM_INT({ Module['printErr']('getcwd(buf=0x' + $0 + ', size= ' + $1 + ')') }, buf, size);

	if (!buf && size > 0) RETURN_ERRNO(EFAULT, "buf points to a bad address");
	if (buf && size == 0) RETURN_ERRNO(EINVAL, "The size argument is zero and buf is not a null pointer");

	inode *cwd = get_cwd();
	if (!cwd) RETURN_ERRNO(-1, "ASMFS internal error: no current working directory?!");
	// TODO: RETURN_ERRNO(ENOENT, "The current working directory has been unlinked");
	// TODO: RETURN_ERRNO(EACCES, "Permission to read or search a component of the filename was denied");
	inode_abspath(cwd, buf, size);
	if (strlen(buf) >= size-1) RETURN_ERRNO(ERANGE, "The size argument is less than the length of the absolute pathname of the working directory, including the terminating null byte.  You need to allocate a bigger array and try again");

	return 0;
}

} // ~extern "C"
