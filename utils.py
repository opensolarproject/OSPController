#! /usr/bin/env python
import os, sys, subprocess

def shellCmd(cmd):
  return subprocess.check_output(cmd.split(' ')).strip().decode("utf-8")

def getDescribe():
  return shellCmd("git describe --long --tags --always --dirty")
def getGitDate():
  return shellCmd("git log -1 --date=format:%Y%m%d --format=%ad")
def getVersion():
  try:
    return getDescribe().replace("-dirty", ".d") + "-" + str(getGitDate())
  except Exception as e:
    return os.path.basename(os.getcwd())
def prettyPrint():
  try: #optional colorful output
    from colorama import Fore, Back, Style
    print(Back.YELLOW + Fore.BLACK + " git version " + Back.BLACK + Fore.YELLOW + " " + getVersion() + " " + Style.RESET_ALL)
  except Exception:
    print("git version " + getVersion())


arg = sys.argv[1] if len(sys.argv) > 1 else ""

if arg == "version":
  prettyPrint()
elif arg == "simple":
  print(getVersion())

else:
  prettyPrint()

  try: #if running inside platformio
    Import("env")
    # print(env.Dump()) # <- can use this to see what's available
    bpath = os.path.join(env.subst("$BUILD_DIR"), "generated")
    print(" - version injection to " + bpath)

    if not os.path.exists(bpath): os.makedirs(bpath)
    with open(os.path.join(bpath, "version.cpp"), 'w+') as ofile:
      ofile.write("const char* GIT_VERSION(\"" + getVersion() + "\");" + os.linesep)
    env.BuildSources(os.path.join(bpath, "build"), bpath)
  except NameError:
    pass
