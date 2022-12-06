#! /usr/bin/env python
# encoding: utf-8
# Sylvain Cetre
import os
# the following two variables are used by the target "waf dist"
VERSION='0.0.1'
APPNAME='daoTools'

# if you want to cross-compile, use a different command line:
# CC=mingw-gcc AR=mingw-ar waf configure build

top = '.'

from waflib import Configure, Logs, Utils, Context
#Configure.autoconfig = True # True/False/'clobber'

def options(opt):
	opt.load('compiler_c gnu_dirs')

def configure(conf):
	conf.load('compiler_c gnu_dirs')
	conf.write_config_header('config.h')
	print('→ prefix is ' + conf.options.prefix)

def build(bld):
	bld.env.DEFINES=['WAF=1']
	bld.recurse('apps')
	bld.recurse('src')
	bld.install_files(bld.env.PREFIX+'/include', 'include/daoTools.h', relative_trick=False)
	bld.install_files(bld.env.PREFIX+'/bin', 'apps/daoImageRTD.py', chmod=0o755, relative_trick=False)
	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoImDisp.py', chmod=0o755, relative_trick=False)
	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoImDisp.ui', relative_trick=False)
	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoRTDMagic.py', relative_trick=False)
	bld.install_files(bld.env.PREFIX+'/bin', 'scripts/daoEnableHT', chmod=0o755, relative_trick=False)
	bld.install_files(bld.env.PREFIX+'/bin', 'scripts/daoDisableHT', chmod=0o755, relative_trick=False)