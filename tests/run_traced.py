"""Run the existing test files with TracedRouter(check=True) in place of gridrouter.Router.

    python3 tests/run_traced.py            (GRIDROUTER_TEST_SCALE=0.2 for a quick run)

Each test file runs in its own Python, as run_all.py does, unchanged.  Before
it starts, gridrouter.Router is replaced by a subclass of
gridrouter.recorder.TracedRouter that

  * asserts the trace invariants on every search (check=True);
  * attaches a Recorder writing to a temporary file as soon as it is built;
  * when it is freed (or at the end of the file), closes the recorder, reads
    the file back and checks it: no corrupt chunk, END present,
    tracefile.check() clean, the replayed copper (spec section 8) equal to
    the router's edges, vias, stubs and edge widths, END's failures and rip
    count equal to the router's.

So every test of the suite also tests the recorder, and the suite's own
assertions (identical to the oracle, rip-up identical to a fresh router, ...)
show that tracing and recording change no result.  test_trace.py is not run
here (it compares Router with TracedRouter itself).  Not part of run_all.py:
it is a separate, slower gate.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = ["test_api.py", "test_search.py", "test_ripup.py", "test_kicad_emit.py"]


def parent():
    tmpdir = tempfile.mkdtemp(prefix="gridrouter_run_traced_")
    env = dict(os.environ, GRIDROUTER_TRACED_TMP=tmpdir)
    failed = []
    for f in FILES:
        print("=" * 70, "\n", f, "(TracedRouter, check=True, recorded)", flush=True)
        py = [sys.executable] + (["-B"] if sys.flags.dont_write_bytecode else [])
        if subprocess.call(py + [os.path.abspath(__file__), "--child", f], env=env) != 0:
            failed.append(f)
    print("=" * 70)
    if failed:
        print("FAILED: " + ", ".join(failed) + "   (trace files kept in %s)" % tmpdir)
    else:
        shutil.rmtree(tmpdir, ignore_errors=True)
        print("all test files passed with TracedRouter(check=True) and a recorder")
    return 1 if failed else 0


def child(name):
    import gc
    import itertools
    import runpy
    import traceback
    import weakref

    sys.path.insert(0, os.path.dirname(HERE))
    sys.path.insert(0, HERE)
    import gridrouter
    from gridrouter import tracefile as tf
    from gridrouter.recorder import Recorder, TracedRouter

    tmpdir = os.environ.get("GRIDROUTER_TRACED_TMP") or tempfile.mkdtemp()
    counter = itertools.count()
    live = weakref.WeakSet()
    failures = []
    stats = dict(routers=0, chunks=0, searches=0, checks=0, bytes=0, traced_entries=0)

    def state_of(r):
        edges = {n: {l: set(s) for l, s in d.items() if s} for n, d in r.edges.items()}
        return ({n: d for n, d in edges.items() if d}, {n: set(s) for n, s in r.vias.items() if s},
                {n: sorted(s) for n, s in r.stubs.items() if s}, dict(r.ewidth))

    def state_of_replay(st):
        edges = {n: {l: set(s) for l, s in d.items() if s} for n, d in st.edges.items()}
        return ({n: d for n, d in edges.items() if d},
                {n: set(s) for n, s in st.vias.items() if s},
                {n: sorted(x[:5] for x in s) for n, s in st.stubs.items() if s}, dict(st.ewidth))

    def verify(r):
        if r.__dict__.get("_gate_done", True):
            return
        r._gate_done = True
        path = r._gate_path
        try:
            want = state_of(r)
            failed = [(str(a), str(b)) for a, b in r.failed]
            rips = r.rips
            r._gate_rec.close()
            tr = tf.read_trace(path)
            probs = list(tr.errors) + tf.check(tr)
            if not tr.complete:
                probs.append("no END chunk")
            got = state_of_replay(tf.replay(tr))
            for k, what in enumerate(("edges", "vias", "stubs", "edge widths")):
                if got[k] != want[k]:
                    probs.append("replayed %s differ from the router's" % what)
            end = [c.data for c in tr.chunks if c.tag == "END "]
            if end and (end[0].failed != failed or end[0].rips != rips):
                probs.append("END failures / rips differ from the router's")
            stats["routers"] += 1
            stats["chunks"] += len(tr.chunks)
            stats["bytes"] += tr.size
            stats["checks"] += r.trace_checks
            for c in tr.chunks:
                if c.tag == "SRCH" and c.data is not None:
                    stats["searches"] += 1
                    stats["traced_entries"] += c.data.ntrace
            if probs:
                failures.append((path, probs[:8]))
            else:
                os.remove(path)
        except Exception:
            failures.append((path, [traceback.format_exc()]))

    class GateRouter(TracedRouter):
        def __init__(self, *args, **kw):
            kw.setdefault("check", True)
            super().__init__(*args, **kw)
            self._gate_path = os.path.join(tmpdir, "%s.%d.%d.grtrace" % (
                os.path.splitext(name)[0], os.getpid(), next(counter)))
            self._gate_rec = Recorder(self._gate_path)
            self._gate_rec.attach(self)
            self._gate_done = False
            live.add(self)

        def __del__(self):
            if "_gate_done" in self.__dict__:
                verify(self)
            super().__del__()

    gridrouter.Router = GateRouter
    code = 0
    sys.argv = [os.path.join(HERE, name)]      # as if run directly (unittest.main reads argv)
    try:
        runpy.run_path(sys.argv[0], run_name="__main__")
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
    except BaseException:
        traceback.print_exc()
        code = 1
    gc.collect()
    for r in list(live):
        verify(r)
    print("recorder gate: %d routers recorded and read back, %d chunks, %d searches "
          "(%d traced entries, %d checked by check_trace), %.1f MB written"
          % (stats["routers"], stats["chunks"], stats["searches"], stats["traced_entries"],
             stats["checks"], stats["bytes"] / 1e6))
    for path, probs in failures[:10]:
        print("RECORDER GATE FAILURE in %s:" % path)
        for p in probs:
            print("   ", p)
    if failures:
        print("%d recorded routers failed the read-back checks" % len(failures))
        code = code or 1
    sys.stdout.flush()
    return code


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--child":
        sys.exit(child(sys.argv[2]))
    sys.exit(parent())
