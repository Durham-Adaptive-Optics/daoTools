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
	opt.load('compiler_c compiler_cxx gnu_dirs')

def configure(conf):
	conf.load('compiler_c compiler_cxx gnu_dirs')
	conf.load('build_tools.pkg_tool')
	conf.write_config_header('config.h')
	print('→ prefix is ' + conf.options.prefix)

	conf.check_cfg(package='protobuf',
				args='--cflags --libs',
				uselib_store='PROTOBUF'
				)

	conf.check_cfg(package='dao',
				args='--cflags --libs',
				uselib_store='DAO'
				)
 
	conf.check_cfg(package='daoNuma',
				args='--cflags --libs',
				uselib_store='DAONUMA'
				)
	conf.check_cfg( package='daoProto',
				args='--cflags --libs',
				uselib_store='DAOPROTO'
				)
	conf.env.PYTHONDIR		= f'{conf.env.PREFIX}/python'
	conf.env.DATADIR		= f'{conf.env.PREFIX}/data'
	conf.env.PKGCONFIGDIR	= f'{conf.env.LIBDIR}/pkgconfig'
 
def build(bld):
	bld.env.DEFINES=['WAF=1']
	bld.recurse('src')
	bld.recurse('apps')


	# include
	files = glob.glob('include/*.h')
	for file in files:
		bld.install_files(bld.env.INCLUDEDIR, file, relative_trick=False)
	files = glob.glob('include/*.hpp')
	for file in files:
		bld.install_files(bld.env.INCLUDEDIR, file, relative_trick=False)
	# src
	files = glob.glob('src/*.py')
	for file in files:
		bld.install_files(bld.env.PYTHONDIR, file, relative_trick=False)
	files = glob.glob('src/python/*.py')
	for file in files:
		bld.install_files(bld.env.PYTHONDIR, file, relative_trick=False)
	# apps
	files = glob.glob('apps/*.py')
	for file in files:
		bld.install_files(bld.env.BINDIR, file, chmod=0o0755, relative_trick=False)
	# gui
	files = glob.glob('gui/*.ui')
	for file in files:
		bld.install_files(bld.env.DATADIR, file, relative_trick=False)
	files = glob.glob('gui/*.py')
	for file in files:
		bld.install_files(bld.env.BINDIR, file, chmod=0o0755, relative_trick=False)
	# script
	files = glob.glob('scripts/*')
	for file in files:
		bld.install_files(bld.env.BINDIR, file, chmod=0o0755, relative_trick=False)

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