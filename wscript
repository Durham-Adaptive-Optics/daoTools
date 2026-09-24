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

from waflib import Configure, Logs, Utils, Context, Task, TaskGen
from waflib.Tools import c_preproc
#Configure.autoconfig = True # True/False/'clobber'

# CUDA sources (.cu) compiled by nvcc (same as waf's optional extras/cuda.py);
# flags in env.CUDAFLAGS, set where the GPU library is built (src/wscript_build).
class cuda(Task.Task):
	run_str = '${NVCC} ${CUDAFLAGS} ${CPPPATH_ST:INCPATHS} ${DEFINES_ST:DEFINES} -c ${SRC} -o ${TGT}'
	color = 'GREEN'
	ext_in = ['.h']
	vars = ['CCDEPS']
	scan = c_preproc.scan
	shell = False

@TaskGen.extension('.cu')
def cuda_hook(self, node):
	return self.create_compiled_task('cuda', node)

def options(opt):
	opt.load('compiler_c compiler_cxx gnu_dirs')

def configure(conf):
	conf.load('compiler_c compiler_cxx gnu_dirs')
	conf.write_config_header('config.h')
	print('→ prefix is ' + conf.options.prefix)

	# Optional C++ application dependencies must not block the core C tools.
	for package, uselib in (
		('protobuf', 'PROTOBUF'),
		('cfitsio', 'cfitsio'),
		('yaml-cpp', 'YAMLCPP'),
		('fmt', 'FMT'),
		('libzmq', 'ZMQ'),
	):
		conf.env[uselib.upper() + '_AVAILABLE'] = bool(conf.check_cfg(
			package=package, args='--cflags --libs',
			uselib_store=uselib, mandatory=False))

	# daoDAQ requires CFITSIO's unsigned 64-bit image/data constants.
	# A .pc file alone may refer to headers that are too old or shadowed.
	if conf.env.CFITSIO_AVAILABLE:
		conf.env.CFITSIO_AVAILABLE = bool(conf.check_cxx(
			fragment="""#include <fitsio.h>
int main() {
    float version;
    fits_get_version(&version);
    return TULONGLONG == 0 || ULONGLONG_IMG == 0;
}""",
			uselib='cfitsio', mandatory=False,
			msg='Checking for usable CFITSIO with unsigned 64-bit support'))

	# Enable draft API support
	conf.env.CFLAGS += ['-DZMQ_BUILD_DRAFT_API']
	conf.env.CXXFLAGS += ['-DZMQ_BUILD_DRAFT_API']

	# Both consumers include CLI/CLI.hpp; verify that exact include path,
	# even when pkg-config reports CLI11 as installed.
	conf.check_cfg(package='CLI11', args='--cflags --libs',
		uselib_store='CLI11', mandatory=False)
	conf.env.HAVE_CLI11 = bool(conf.check_cxx(
		header_name='CLI/CLI.hpp', uselib='CLI11', mandatory=False))

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

	# Check for BLAS (try both 'blas' and 'openblas' pkg-config names)
	conf.env.BLAS_AVAILABLE = False  # Default to False
	try:
		conf.check_cfg(package='blas', args='--cflags --libs', uselib_store='BLAS')
		conf.env.BLAS_AVAILABLE = True
		print("BLAS detected: enabling BLAS build.")
	except:
		try:
			conf.check_cfg(package='openblas', args='--cflags --libs', uselib_store='BLAS')
			conf.env.BLAS_AVAILABLE = True
			print("OpenBLAS detected: enabling BLAS build.")
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
	bld.recurse('tests')

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
	# config
	files = glob.glob('config/daq/*.yaml')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/config/daq', file, relative_trick=False)
	# default daoDAQ root_storage (config/daq/default.yaml): ${DAOROOT}/telemetry
	if bld.cmd == 'install':
		bld.add_post_fun(make_telemetry_dir)
	# script
	files = glob.glob('scripts/*')
	for file in files:
		bld.install_files(bld.env.PREFIX+'/bin', file, chmod=0o0755, relative_trick=False)

def make_telemetry_dir(ctx):
	from waflib import Options
	path = os.path.join(ctx.env.PREFIX, 'telemetry')
	if Options.options.destdir:
		path = os.path.join(Options.options.destdir, path.lstrip(os.sep))
	os.makedirs(path, exist_ok=True)

docs = 'docs'

def build_docs(conf):
	os.system("doxygen docs/Doxyfile")
	os.system(f"make -C {docs} html")

def clean_docs(conf):
	os.system(f"make -C {docs} clean")
