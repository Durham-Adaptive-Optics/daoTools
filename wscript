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
	conf.write_config_header('config.h')
	print('→ prefix is ' + conf.options.prefix)

	conf.check_cfg( package='protobuf',
				args='--cflags --libs',
				uselib_store='PROTOBUF'
				)
 
	conf.check_cfg( package='cfitsio',
				args='--cflags --libs',
				uselib_store='cfitsio'
				)
 
	conf.check_cfg(package='yaml-cpp',
				args='--cflags --libs',
				uselib_store='YAMLCPP'
				)
 
	conf.check_cfg(package='fmt',
				args='--cflags --libs',
				uselib_store='FMT'
				)
 
	# Check for ZeroMQ
	conf.check_cfg(package='libzmq',
				args='--cflags --libs',
				uselib_store='ZMQ'
				)
 
	# Enable draft API support
	conf.env.CFLAGS += ['-DZMQ_BUILD_DRAFT_API']
	conf.env.CXXFLAGS += ['-DZMQ_BUILD_DRAFT_API']

	# --- CLI11 (header-only) ---
	conf.env.HAVE_CLI11 = False
	# Try pkg-config if the distro provides it (some do)
	try:
		conf.check_cfg(package='CLI11', args='--cflags --libs', uselib_store='CLI11')
		conf.env.HAVE_CLI11 = True
	except:
		# Fallback: just verify the header exists from libcli11-dev
		if conf.check_cxx(header_name='CLI11.hpp', mandatory=False):
			conf.env.HAVE_CLI11 = True
		elif conf.check_cxx(header_name='CLI/CLI.hpp', mandatory=False):
			conf.env.HAVE_CLI11 = True
		else:
			conf.fatal('CLI11 not found. Install libcli11-dev or provide CLI11.hpp')

	# Check for CUDA
	conf.env.CUDA_AVAILABLE = False  # Default to False
	try:
		nvcc_path = conf.find_program('nvcc', var='NVCC')
		if isinstance(nvcc_path, list):
			nvcc = nvcc_path[0]
		else:
			nvcc = nvcc_path
		conf.env.CUDA_PATH = os.path.dirname(os.path.dirname(nvcc))
		conf.env.INCLUDES_CUDA = [os.path.join(conf.env.CUDA_PATH, 'include')]
		conf.env.LIBPATH_CUDA = [os.path.join(conf.env.CUDA_PATH, 'lib64')]
		conf.env.LIB_CUDA = ['cudart']

		# Check that cuda_runtime.h exists
		conf.check(header_name='cuda_runtime.h', includes=conf.env.INCLUDES_CUDA)
		# Check that cudart lib is there
		conf.check_cxx(lib='cudart', libpath=conf.env.LIBPATH_CUDA)

		conf.env.CUDA_AVAILABLE = True
		print('CUDA detected: enabling GPU build.')
	except Exception as e:
		print('CUDA not found or incomplete: skipping GPU.', e)

	# Check for BLAS
	conf.env.BLAS_AVAILABLE = False  # Default to False
	try:
		conf.check_cfg(package='blas', args='--cflags --libs', uselib_store='BLAS')
		conf.env.BLAS_AVAILABLE = True
		print("BLAS detected: enabling BLAS build.")
	except:
		print("BLAS not found: skipping BLAS.")

	# Check for FFTW (single- and double-precision; both required to enable
	# the FFT-based correlation centroider, daoToolsCorrFFT).
	conf.env.FFTW_AVAILABLE = False  # Default to False
	try:
		conf.check_cfg(package='fftw3f', args='--cflags --libs', uselib_store='FFTW3F')
		conf.check_cfg(package='fftw3',  args='--cflags --libs', uselib_store='FFTW3')
		conf.env.FFTW_AVAILABLE = True
		print("FFTW (single+double) detected: enabling FFT correlation centroider build.")
	except:
		print("FFTW not found (need both fftw3f and fftw3): skipping FFT correlation centroider.")

def build(bld):
	bld.env.DEFINES=['WAF=1']
	bld.recurse('src')
	bld.recurse('apps')

	# include
	files = glob.glob('include/*.h')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/include', file, relative_trick=False)
	files = glob.glob('include/*.hpp')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/include', file, relative_trick=False)
	# src
	files = glob.glob('src/*.py')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/python', file, relative_trick=False)
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

docs = 'docs'

def build_docs(conf):
	os.system("doxygen docs/Doxyfile")
	os.system(f"make -C {docs} html")

def clean_docs(conf):
	os.system(f"make -C {docs} clean")
