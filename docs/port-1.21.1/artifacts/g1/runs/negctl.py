# Makes a scratch copy of natives/ (without build/) and LICENSES/ under argv[1], with the ExceptionCheck after
# CallObjectMethod removed from natives/src/jni_machine.c (G1 item 4, the negative control). The repo is not touched.
import pathlib, shutil, sys
repo = pathlib.Path(r"<repo>")
dest = pathlib.Path(sys.argv[1])
if dest.exists():
    shutil.rmtree(dest)
shutil.copytree(repo / "natives", dest / "natives", ignore=shutil.ignore_patterns("build"))
shutil.copytree(repo / "LICENSES", dest / "LICENSES")
src = dest / "natives" / "src" / "jni_machine.c"
text = src.read_bytes().decode("ascii")
block = ("  if ((*env)->ExceptionCheck(env)) {\n"
         "    (*env)->ExceptionDescribe(env);   /* prints the exception and its stack trace to stderr, and clears it */\n"
         "    rc = CM_HOST_E_JAVA;\n"
         "    goto out;\n"
         "  }\n")
anchor = "(*env)->CallObjectMethod(env, host->self, host->upcall, (jint)fn, jreq);\n"
assert text.count(anchor + block) == 1, "block after CallObjectMethod not found exactly once"
text = text.replace(anchor + block, anchor + "  /* NEGATIVE CONTROL: the ExceptionCheck after CallObjectMethod is removed */\n")
src.write_bytes(text.encode("ascii"))
print("mutant written:", src)
