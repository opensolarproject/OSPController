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
    return getDescribe() + "-" + str(getGitDate())
  except Exception as e:
    return os.path.basename(os.getcwd())


if len(sys.argv) > 1 and sys.argv[1] == "version":
  print("-DGIT_VERSION=\\\"%s\\\"" % getVersion())

elif len(sys.argv) > 1 and sys.argv[0].endswith("scons"): #pre/post extra_script
  Import("env")
  # print(Back.GREEN + Fore.BLACK + "sys argv" + Style.RESET_ALL + str(sys.argv))
  # print(Back.GREEN + Fore.BLACK + "env dump" + Style.RESET_ALL + str(env.Dump()))
  # print(Back.GREEN + Fore.BLACK + "projenv" + Style.RESET_ALL + str(projenv.Dump()))
  try: #optional colorful output
    from colorama import Fore, Back, Style
    print(Back.YELLOW + Fore.BLACK + " solar version " + Back.BLACK + Fore.YELLOW + " " + getVersion() + " " + Style.RESET_ALL)
  except Exception:
    print("solar version " + getVersion())
  # -- not working, breaks partition.bin for some reason -- #
  # progname = "solar-%s-%s" % (env.get("BOARD"), getVersion())
  # log(" program name ", progname)
  # env.Replace(PROGPREFIX=progname)

