import subprocess
r = subprocess.run(["dpkg-deb", "-I", "/tmp/bulum-repo/hello_1.0_arm64.deb"],
                   capture_output=True, text=True)
print("RC", r.returncode, "OUTLEN", len(r.stdout))
print(repr(r.stdout[:200]))
print("ERR", repr(r.stderr[:100]))