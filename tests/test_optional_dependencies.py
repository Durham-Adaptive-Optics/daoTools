"""Build selection checks without requiring optional packages on the test host."""
import ast
from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = {
    'protobuf': 'PROTOBUF_AVAILABLE',
    'cfitsio': 'CFITSIO_AVAILABLE',
    'yaml-cpp': 'YAMLCPP_AVAILABLE',
    'fmt': 'FMT_AVAILABLE',
    'libzmq': 'ZMQ_AVAILABLE',
    'CLI11': 'HAVE_CLI11',
}


class Environment(dict):
    def __getattr__(self, key):
        return self.get(key, [])

    def __setattr__(self, key, value):
        self[key] = value


class Configure:
    def __init__(self, missing):
        self.missing = missing
        self.env = Environment(CFLAGS=[], CXXFLAGS=[])
        self.options = Environment(prefix='/tmp/dao-test')
        self.checks = []

    def load(self, *args):
        pass

    def write_config_header(self, *args):
        pass

    def check_cfg(self, **kwargs):
        self.checks.append(kwargs)
        if kwargs['package'] in self.missing:
            if kwargs.get('mandatory', True):
                raise RuntimeError('required package missing')
            return None
        return True

    def check_cxx(self, **kwargs):
        if 'fragment' in kwargs:
            assert kwargs['uselib'] == 'cfitsio'
            assert kwargs['mandatory'] is False
            return 'cfitsio-incompatible' not in self.missing
        assert kwargs['header_name'] == 'CLI/CLI.hpp'
        assert kwargs['mandatory'] is False
        return 'CLI11' not in self.missing

    def find_program(self, *args, **kwargs):
        raise RuntimeError('CUDA unavailable in this fixture')


class Build:
    def __init__(self, env):
        self.env = env
        self.targets = {}
        self.subdirs = []

    def program(self, **kwargs):
        self.targets[kwargs['target']] = kwargs
        return kwargs

    def recurse(self, name):
        self.subdirs.append(name)


class OptionalDependencyTests(unittest.TestCase):
    def test_missing_dependencies_only_skip_consumers(self):
        module = ast.parse((ROOT / 'wscript').read_text())
        configure = next(node for node in module.body
                         if isinstance(node, ast.FunctionDef) and node.name == 'configure')
        namespace = {'os': os}
        exec(compile(ast.Module(body=[configure], type_ignores=[]), 'wscript', 'exec'), namespace)
        scenarios = [set(), *[{package} for package in PACKAGES], set(PACKAGES), {'cfitsio-incompatible'}]
        for missing in scenarios:
            with self.subTest(missing=sorted(missing)), redirect_stdout(io.StringIO()) as output:
                conf = Configure(missing)
                namespace['configure'](conf)
                for package, flag in PACKAGES.items():
                    self.assertEqual(conf.env[flag], package not in missing and not (package == 'cfitsio' and 'cfitsio-incompatible' in missing))
                for check in conf.checks:
                    if check['package'] in PACKAGES:
                        self.assertFalse(check['mandatory'])
                bld = Build(conf.env)
                with patch.dict(os.environ, {'DAOROOT': '/tmp/dao-test'}):
                    exec(compile((ROOT / 'apps/wscript_build').read_text(),
                                 'apps/wscript_build', 'exec'), {'bld': bld})
                self.assertIn('daoClock', bld.targets)
                self.assertIn('daoPixelCalibrate', bld.targets)
                self.assertIn('daoMvM', bld.targets)
                self.assertEqual('daoDAQ' in bld.subdirs, not missing)
                needs_downsample = bool(missing & {'CLI11', 'protobuf', 'libzmq'})
                self.assertEqual('daoDownsample' in bld.targets, not needs_downsample)
                if missing:
                    self.assertIn('Skipping daoDAQ build (missing or unusable:', output.getvalue())
                if needs_downsample:
                    self.assertIn('Skipping daoDownsample build (missing or unusable:', output.getvalue())


if __name__ == '__main__':
    unittest.main()
