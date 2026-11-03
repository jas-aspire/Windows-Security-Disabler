# Windows Security Disabler
### ⚠️I WILL NOT BE PROVIDING ANY BINARIES as this is marked by many antiviruses and compiling yourself is always the better option ###
### Please add imgui-1.92.9 in the parent directory of source files for proper linkage
### As per compiling I would just use claude code or any AGENT to use your compiler to build the executable since I dont have a solution or cmake file for you

# How this works

### I took from [https://github.com/pgkt04/defender-control](https://github.com/pgkt04/defender-control) and used that logic and then made my own PS1 scripts to disable UAC and Core Isolation

What this disables
- Everything in Windows Security Menu
- Memory Integrity
- UAC (Admin access no permission needed, everything elevated)
- HyperVisor Code Integrity
- Microsoft Vuln Driver Blocklist
##### in @resources.h these are just the exes from the other github converted into bytes and packaged into the final executable to make it fully offline


# Where is raw logic?

The raw ps1 files and logic are in the /resources folder. good luck =D



#### THIS CAN BE USED BY ANYONE, WITH OR WITHOUT CREDIT since its all public info.
