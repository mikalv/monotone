This directory keeps the files needed to build the Windows native
installer, using Inno Setup. 

Here are all the steps for creating a MinGW release, on a fresh
Windows machine:

Install Inno Setup and Inno Setup preprocessor; see
http://www.jrsoftware.org/isinfo.php, get the Quick Start Pack isetup-5.4.2.exe

Install MinGW tools; see ../INSTALL_windows_native.txt

Install a monotone binary; see http://monotone.ca/

Get a copy of the monotone repository; see
http://wiki.monotone.ca/MonotoneProjectServer/

Check out the release version of monotone:
mtn -d /path/to/monotone.db checkout -r t:monotone-<version> --branch net.venge.monotone.monotone-<version> monotone-<version>

Build the release. See the last instruction in ../INSTALL_windows_native.txt
Then build the installer:

make win32-installer

That builds "monotone-<version>-setup.exe" in the current directory.

NOTE:

  If ISCC (Inno setup compiler) is not in PATH, then you must specify
  its location:
  
    make win32-installer ISCC=/d/progs/InnoSetup5/ISCC.exe

Publish the binary on the monotone website with proper permissions:

chmod a+r monotone-<version>-setup.exe
scp -p monotone-<version>-setup.exe mtn-uploads@monotone.ca:<version>

Download from the web (http://www.monotone.ca/downloads.php), install
and test, both the installed mtn and the documentation.