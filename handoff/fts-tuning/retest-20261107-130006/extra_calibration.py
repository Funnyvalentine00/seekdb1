"""User-authorized cap extension: at most two more same-version pairs."""
import json
import signal
import time
import traceback

from run_calibration import HERE, Retest, backend, interrupted, power_snapshot


def main():
    assert (HERE / "correctness.rc").read_text().strip() == "0"
    assert json.loads((HERE / "aa-result-resumed.json").read_text())["reports"] == 4
    assert json.loads((HERE / "functional-reference/stopped.json").read_text())["normal"] is True
    tags = ("extra-aa3a", "extra-aa3b", "extra-aa4b", "extra-aa4a")
    assert all(not (HERE / tag).exists() for tag in tags)
    session = Retest()
    session.data = HERE / "data-extra"
    assert not session.data.exists()
    backend.write_json(HERE / "extra-started.json", dict(t_ns=time.time_ns(),
        authorization="user permits additional groups to reduce measurement uncertainty",
        total_report_cap=8, extra_pairs_cap=2, product_experiments=0,
        data_reason="fresh owned data separates extra measurements from regression SQL state"))
    session.preflight()
    backend.write_json(HERE / "extra-power-before.json", power_snapshot())
    completed = []
    try:
        for tag in tags:
            before = session.host_load()
            time.sleep(5)
            begin = session.host_load()
            backend.write_json(HERE / (tag + "-idle.json"), dict(fixed_idle_seconds=5,
                before_idle=before, after_idle=begin))
            if begin["max_load_pct"] > session.cpu_max:
                print("EXTRA_HOST_LOAD_LIMIT", tag, begin["max_load_pct"], flush=True)
                break
            out = HERE / tag
            with session.instance(tag, out) as (process, _):
                rec = session.benchmark(tag, out, process)
                rec.update(host_before=begin, host_after=session.host_load(),
                           fixed_idle_before_start_seconds=5, extra_window=True)
                backend.write_json(out / "official-summary.json", rec)
                backend.write_json(out / "tails.json", session.tails(process.pid))
                completed.append(tag)
                print("EXTRA_AA_COMPLETE", tag, rec["metrics"]["select1_avg_ms"], flush=True)
            if rec["host_after"]["max_load_pct"] > session.cpu_max:
                print("EXTRA_HOST_LOAD_LIMIT_AFTER", tag, rec["host_after"]["max_load_pct"], flush=True)
                break
    finally:
        backend.write_json(HERE / "extra-power-after.json", power_snapshot())
        backend.write_json(HERE / "extra-resource-final.json", session.guard())
    backend.write_json(HERE / "extra-window.json", dict(completed=completed,
        new_pairs=len(completed)//2, original_reports_retained=4,
        total_reports=4+len(completed), no_further_retries=True))
    print("EXTRA_WINDOW_DONE", len(completed), flush=True)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except Exception as exc:
        backend.write_json(HERE / "extra-failure.json", dict(error=repr(exc), traceback=traceback.format_exc()))
        raise
