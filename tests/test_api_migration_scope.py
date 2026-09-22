"""Prove every migrated source differs only by the documented API transformation."""
import os
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[1]
if not os.getenv('DAO_TOOLS_BASELINE_SOURCE'):
    pytest.skip('Set DAO_TOOLS_BASELINE_SOURCE; see tests/README.md', allow_module_level=True)
BASELINE = Path(os.environ['DAO_TOOLS_BASELINE_SOURCE'])
assert BASELINE.is_dir(), f'Missing baseline source directory: {BASELINE}'
RENAMES = {
    'daoShmImageCreate': 'daoShmCreate',
    'daoShmShm2Img': 'daoShmOpen',
    'daoShmImage2Shm': 'daoShmSetData',
    'daoShmImagePart2ShmFinalize': 'daoShmSetDataPartFinalize',
    'daoShmCombineShm2Shm': 'daoShmCombine',
    'daoShmWaitForSemaphore': 'daoShmWaitSem',
    'daoShmWaitForSemaphoreTimeout': 'daoShmWaitSemTimeout',
}
# Keep strings/chars as indivisible tokens and discard comments/whitespace.
TOKEN = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'
    r"'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|\d+(?:\.\d*)?|[^\s]")


def tokens(source):
    return [m.group() for m in TOKEN.finditer(source)
            if not m.group().startswith(('//', '/*'))]


def affected():
    for file in sorted(BASELINE.rglob('*')):
        if file.suffix not in ('.c', '.cpp', '.h', '.hpp'):
            continue
        if any(t in RENAMES for t in tokens(file.read_text())):
            yield file.relative_to(BASELINE)


@pytest.mark.parametrize('relative', list(affected()), ids=str)
def test_only_api_names_and_write_argument_order_changed(relative):
    before = tokens((BASELINE / relative).read_text())
    after = tokens((ROOT / relative).read_text())
    assert not any(t in RENAMES for t in after), 'legacy call remains'
    # Undo the new write signature independently, then compare every token.
    for start in reversed([i for i, t in enumerate(after) if t == 'daoShmSetData']):
        assert after[start + 1] == '('
        depth, args, argument = 0, [], []
        end = start + 2
        while end < len(after):
            t = after[end]
            if t == ')' and depth == 0:
                args.append(argument)
                break
            if t == ',' and depth == 0:
                args.append(argument)
                argument = []
            else:
                argument.append(t)
                depth += (t == '(') - (t == ')')
            end += 1
        assert len(args) == 3
        after[start + 2:end] = args[1] + [','] + args[2] + [','] + args[0]
    inverse = {new: old for old, new in RENAMES.items()}
    after = [inverse.get(t, t) for t in after]
    assert after == before
