# FUSE - File System in User Space

A simple index allocation based file system implemented at the user level using the FUSE library.

The File system has the following properties:
1. The root directory “/” will only contain other subdirectories, and no regular files.
2. The subdirectories will only contain regular files, and no subdirectories of their own.
3. All files will be full access (i.e., chmod 0666), with permissions to be mainly ignored.
4. Many file attributes such as creation and modification times will not be accurately stored.
5. Files cannot be truncated.

All file system contents are contained in a disk image ``.disk``. 
