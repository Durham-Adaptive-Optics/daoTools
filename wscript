#! /usr/bin/env python
# encoding: utf-8
# Sylvain Cetre
import os
import glob
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


	# include
	files = glob.glob('include/*.h')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/include', file, relative_trick=False)
	files = glob.glob('include/*.hpp')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/include', file, relative_trick=False)
	# src
	files = glob.glob('src/python/*.py')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/python', file, relative_trick=False)
	# apps
	files = glob.glob('apps/*.py')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/bin', file, chmod=0o0755, relative_trick=False)
	# gui
	files = glob.glob('gui/*.ui')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/data', file, relative_trick=False)
	files = glob.glob('gui/*.py')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/bin', file, chmod=0o0755, relative_trick=False)
	# script
	files = glob.glob('scripts/*')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/bin', file, chmod=0o0755, relative_trick=False)

#	bld.install_files(bld.env.PREFIX+'/include', 'include/daoTools.h', relative_trick=False)
#
#	bld.install_files(bld.env.PREFIX+'/python', 'src/daoTools.py', relative_trick=False)
#
#	bld.install_files(bld.env.PREFIX+'/bin', 'scripts/daoEnableHT', chmod=0o755, relative_trick=False)
#	bld.install_files(bld.env.PREFIX+'/bin', 'scripts/daoDisableHT', chmod=0o755, relative_trick=False)
#
#	bld.install_files(bld.env.PREFIX+'/bin', 'apps/daoImageRTD.py', chmod=0o755, relative_trick=False)
#	bld.install_files(bld.env.PREFIX+'/bin', 'apps/daoReceiveLogs.py', relative_trick=False)
#	bld.install_files(bld.env.PREFIX+'/bin', 'apps/daoSendLogs.py', relative_trick=False)
#
#	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoImDisp.py', chmod=0o755, relative_trick=False)
#	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoImDisp.ui', relative_trick=False)
#	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoRTDMagic.py', chmod=0o755, relative_trick=False)
#	bld.install_files(bld.env.PREFIX+'/bin', 'gui/daoRTDMagic.py', chmod=0o755, relative_trick=False)