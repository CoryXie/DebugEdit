1. Introduction

An ELF file debug information editor based on debugedit tool in the rpm package: 

http://www.rpm.org/

The intention of this project is to have some fun playing with ELF as well as
DWARF for the image files. 

One specific goal for now is to allow the editing of images compiled on Linux 
or Cygwin environment to be debugged on Windows (assuming the same layout of
source directory is available). I am not sure if there is already such a tool,
becasue when I debug an image compiled under Cygwin with Eclipse CDT, and when
I tried to "Edit Source Look Up", the sources are still not loaded, leaving me
the assmbler window! Peeking into the ELF sections, I saw paths like this:

/cygdrive/E/Work/xvisor/emulators/sys/arm_sysregs.c

I think this is due to the lack of correct mapping between the Cygwin paths and
the Windows paths. The existing debugedit tool can replace the path base_dir to
dest_dir, but it lacks a way to change '/' to '\' for mapping between Linux or
Cygwin environment to Windows environment. So I would like to add a command 
option for such a feature (turn this option on by '-w' option). 

Meanwhile, during working on this tool, I found some issues with it and I would 
like to fix these issues.

The rpm specific code has been removed from this tool so that it is pure enough.

2. Usage

#./debugedit.exe -w -b "/cygdrive/E/Work/xvisor" -d "E:\Work\xvisor" vmm.elf 