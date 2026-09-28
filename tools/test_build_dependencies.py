#!/usr/bin/env python3
"""Real Clang compilation proves transitive/command-aware incremental builds.

Only temporary fixtures/objects are created. The kernel and user's existing
build outputs are never touched. No synthetic object or mocked compiler result
is used to establish cache correctness.
"""
from pathlib import Path
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('kestrel_build_dependencies',ROOT/'build.py')
build=importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


@contextlib.contextmanager
def temporary_fixtures():
    # Use ordinary inherited permissions, not TemporaryDirectory's private
    # Windows DACL: the regression must model the shared build output directory.
    parent=Path(tempfile.gettempdir()).resolve()
    path=parent/('kestrel dependency fixtures '+uuid.uuid4().hex)
    path.mkdir(mode=0o777)
    try:yield str(path)
    finally:
        assert path.resolve().parent==parent and path.name.startswith('kestrel dependency fixtures ')
        shutil.rmtree(path) # unique, owned test fixture; never a repository path


def check_inherited_access(out,objects):
    if os.name!='nt':return
    paths=[out]+[p for obj in objects for p in (obj,Path(str(obj)+'.d'),Path(str(obj)+'.deps.json'))]
    literals=','.join("'"+str(p).replace("'","''")+"'" for p in paths)
    script=r'''
$ErrorActionPreference='Stop'
$env:PSModulePath=Join-Path $PSHOME 'Modules'
$paths=@(PATHS)
@($paths | ForEach-Object {
    $acl=Get-Acl -LiteralPath $_ -ErrorAction Stop
    [pscustomobject]@{path=$_;protected=$acl.AreAccessRulesProtected;rules=@($acl.Access | ForEach-Object {
        [pscustomobject]@{sid=$_.IdentityReference.Translate([System.Security.Principal.SecurityIdentifier]).Value;
            inherited=$_.IsInherited;inheritance=[int]$_.InheritanceFlags;type=[int]$_.AccessControlType;rights=[int]$_.FileSystemRights}
    })}
}) | ConvertTo-Json -Depth 5 -Compress
'''.replace('PATHS',literals)
    result=subprocess.run(['powershell','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-Command',script],
                          check=True,capture_output=True,text=True)
    assert not result.stderr,result.stderr
    records=json.loads(result.stdout)
    # ObjectInherit ACEs must reach each published file. CREATOR OWNER becomes
    # the creator's SID during inheritance, so it is not compared literally.
    expected={r['sid'] for r in records[0]['rules'] if r['inheritance']&2 and r['type']==0 and r['sid']!='S-1-3-0'}
    assert expected,('fixture output directory must provide inheritable access',records[0])
    for record in records[1:]:
        inherited={r['sid'] for r in record['rules'] if r['inherited'] and r['type']==0}
        assert not record['protected'] and expected<=inherited,(record,expected)
    print('PASS Windows Get-Acl: object, dependency and manifest inherit output-directory access (no private staging DACL)')


def main():
    actual_run=build.run
    commands=[]
    def checked_run(cmd,quiet=False):
        commands.append(cmd)
        return actual_run(cmd,quiet)
    build.run=checked_run
    # Explicit make-escaping examples supplement the compiler's real Windows
    # output below. A backslash before an ordinary letter is a path separator.
    assert build._compiler_dependencies(
        'kestrel_object: C:\\tree\\file.c C:/some\\ dir/a\\#b$$c.h \\\n D:/nest\\ dir/f.h\n')==[
        'C:\\tree\\file.c','C:/some dir/a#b$c.h','D:/nest dir/f.h']
    for invalid in ['', 'C:/obj.o: input.c', 'kestrel_object:', 'kestrel_object: bad#comment']:
        try:build._compiler_dependencies(invalid)
        except ValueError:pass
        else:raise AssertionError(invalid)
    with temporary_fixtures() as tmp:
        root=Path(tmp);inc=root/'include paths';nested=inc/'nested headers';tools=root/'generated tools'
        assert build.compile_all([],str(root/'empty outputs'),[])==[]  # no compiler needed
        out=root/'object outputs';nested.mkdir(parents=True);tools.mkdir()
        top=inc/'top header.h';deep=nested/'deep #$ value.h';shader=tools/'shader generated.h'
        source=root/'source with spaces.c';assembly=root/'startup with spaces.S';asm_header=inc/'asm value.h'
        top.write_text('#include "nested headers/deep #$ value.h"\n')
        deep.write_text('#include "../../generated tools/shader generated.h"\n#define DEEP_VALUE 10\n')
        shader.write_text('#define GENERATED_VALUE 20\n')
        asm_header.write_text('#define ASM_VALUE 123\n')
        source.write_text('#include "top header.h"\nint test_value(void){return GENERATED_VALUE+DEEP_VALUE+BUILD_FLAVOUR;}\n')
        assembly.write_text('#include "asm value.h"\n.text\n.globl test_asm\ntest_asm:\n.long ASM_VALUE\n')
        flags=['--target=x86_64-unknown-none-elf','-ffreestanding','-O1','-DBUILD_FLAVOUR=1','-I',str(inc)]
        def compile_expected(n,sources=None,options=None,deps=()):
            before=len(commands)
            with contextlib.redirect_stdout(io.StringIO()):
                result=build.compile_all(sources or [source,assembly],str(out),options or flags,deps)
            assert len(commands)-before==n,(n,commands[before:])
            assert not [p for p in out.iterdir() if p.is_dir() or p.name.startswith('.')], 'temporary compile output survived'
            return list(map(Path,result))
        objects=compile_expected(2);c_obj,s_obj=objects
        compile_expected(0)
        check_inherited_access(out,objects)
        manifest=Path(str(c_obj)+'.deps.json');depfile=Path(str(c_obj)+'.d')
        saved=json.loads(manifest.read_text());inputs={p for p,_ in saved['inputs']}
        assert all(str(p.resolve()) in inputs for p in [source,top,deep,shader])
        assert saved['command']['compiler'][1]==digest(saved['command']['compiler'][0])
        assert str(deep.resolve()) in [os.path.abspath(p) for p in build._compiler_dependencies(depfile.read_text())]
        # Old manifests may describe files carrying private staging ACLs. They
        # must be rebuilt, rather than treating their contents as a cache hit.
        saved['version']=1;manifest.write_text(json.dumps(saved))
        compile_expected(1);compile_expected(0)
        assert json.loads(manifest.read_text())['version']==2
        check_inherited_access(out,objects)
        print('PASS real C/.S dependency paths: spaces, hash, dollar, drive/path separators, nested generated shader headers')

        old=digest(c_obj);deep.write_text(deep.read_text().replace('VALUE 10','VALUE 11'))
        compile_expected(1);assert digest(c_obj)!=old;compile_expected(0)
        # Regenerated SASS-like header outside all explicit dep directories.
        # Preserve timestamp and length: content, not newest mtime, is decisive.
        before=shader.stat();old=digest(c_obj)
        shader.write_text('#define GENERATED_VALUE 21\n')
        os.utime(shader,ns=(before.st_atime_ns,before.st_mtime_ns))
        compile_expected(1);assert digest(c_obj)!=old;compile_expected(0)
        old=digest(c_obj);flags[3]='-DBUILD_FLAVOUR=7'
        compile_expected(2);assert digest(c_obj)!=old;compile_expected(0)
        old=digest(c_obj);source.write_text(source.read_text().replace('DEEP_VALUE+','DEEP_VALUE*2+'))
        compile_expected(1);assert digest(c_obj)!=old;compile_expected(0)
        old=digest(s_obj);asm_header.write_text('#define ASM_VALUE 456\n')
        compile_expected(1);assert digest(s_obj)!=old;compile_expected(0)
        # Content-identical timestamp changes are not a reason to recompile.
        os.utime(deep,None);compile_expected(0)
        print('PASS nested/source/generated-header/.S edits, preserved timestamps, flag change, unchanged second runs')

        def failed_compile():
            previous=c_obj.read_bytes();before=len(commands)
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    build.compile_all([source],str(out),flags)
            except SystemExit as error:assert error.code==1
            else:raise AssertionError('compiler failure unexpectedly accepted')
            assert len(commands)==before+1
            assert c_obj.read_bytes()==previous, 'failed compile damaged previous object'
            assert not manifest.exists(), 'failed compile retained trusted cache metadata'
            assert not [p for p in out.iterdir() if p.is_dir() or p.name.startswith('.')]
        header_bytes=shader.read_bytes();shader.unlink();failed_compile()
        shader.write_bytes(header_bytes);compile_expected(1);compile_expected(0)
        valid_source=source.read_text();source.write_text(valid_source+'\nthis is not C;\n');failed_compile()
        source.write_text(valid_source);compile_expected(1);compile_expected(0)
        # Even restoring precisely the old successful flags cannot reuse an
        # object after a failed compiler attempt removed its commit marker.
        flags.append('-this-is-not-a-valid-clang-option');failed_compile();flags.pop()
        compile_expected(1);compile_expected(0)
        manifest.write_text('{interrupted json');compile_expected(1);compile_expected(0)
        manifest.unlink();compile_expected(1);compile_expected(0)
        print('PASS missing include, syntax/flag failure, atomic old-object preservation, restored retry, corrupt/absent manifest')

        # Let real Clang finish, then edit an included header before build.py
        # publishes the object. Reject this build now, not just its future cache.
        previous=c_obj.read_bytes();previous_dep=depfile.read_bytes();before=len(commands)
        def compile_then_edit(cmd,quiet=False):
            result=checked_run(cmd,quiet)
            deep.write_text(deep.read_text().replace('VALUE 11','VALUE 19'))
            changed_at=time.time_ns();os.utime(deep,ns=(changed_at,changed_at))
            return result
        build.run=compile_then_edit;flags.append('-DBUILD_RACE=1')
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                build.compile_all([source],str(out),flags)
        except RuntimeError as error:
            assert 'inputs changed during compilation' in str(error)
        else:raise AssertionError('known inconsistent object was published')
        finally:
            build.run=checked_run;flags.pop()
        assert len(commands)==before+1 and c_obj.read_bytes()==previous
        assert depfile.read_bytes()==previous_dep and not manifest.exists()
        assert not [p for p in out.iterdir() if p.is_dir() or p.name.startswith('.')]
        compile_expected(1);assert c_obj.read_bytes()!=previous;compile_expected(0)
        print('PASS detected concurrent include edit aborts current build before publishing, preserves previous object/depfile, clean retry')

        explicit=root/'explicit prerequisite';explicit.write_text('one')
        compile_expected(2,deps=[str(explicit)]);compile_expected(0,deps=[str(explicit)])
        explicit.write_text('two');compile_expected(2,deps=[str(explicit)]);compile_expected(0,deps=[str(explicit)])
        compile_expected(2) # changed declared prerequisite set is part of command
        env=os.environ.get('CPATH')
        try:
            os.environ['CPATH']=str(inc);compile_expected(2);compile_expected(0)
        finally:
            if env is None:os.environ.pop('CPATH',None)
            else:os.environ['CPATH']=env
        compile_expected(2);compile_expected(0)
        # A different compiler executable path must invalidate the cache even
        # when it is byte-for-byte the same real Clang binary.
        original_clang=build.CLANG
        binary=Path(shutil.which(original_clang) or original_clang)
        alias=root/('alternate clang'+binary.suffix)
        try:os.link(binary,alias)
        except OSError:shutil.copy2(binary,alias)
        try:
            build.CLANG=str(alias);compile_expected(2);compile_expected(0)
        finally:build.CLANG=original_clang
        compile_expected(2);compile_expected(0)
        print('PASS explicit prerequisites, include environment, real compiler identity/command changes')
        print('PASS compiler dependency cache integration: %u real Clang invocations; temporary fixtures only' % len(commands))


if __name__=='__main__':
    main()
