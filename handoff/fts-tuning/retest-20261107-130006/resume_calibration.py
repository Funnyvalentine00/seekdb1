"""One bounded continuation: retain first report, add fixed idle before remaining runs."""
import json
import signal
import time
import traceback

from run_calibration import HERE, Retest, analyze, backend, interrupted, power_snapshot


def main():
    previous = json.loads((HERE / "power-aa1a/stopped.json").read_text())
    assert previous["normal"] is True and previous["returncode"] == 0
    assert json.loads((HERE / "run.rc").read_text()) == 0
    assert json.loads((HERE / "aa-result.json").read_text())["reports"] == 1
    tags = ("power-aa1b", "power-aa2b", "power-aa2a")
    assert all(not (HERE / tag).exists() for tag in tags)
    backend.write_json(HERE / "resume-started.json", dict(t_ns=time.time_ns(),
        reason="fixed five-second idle avoids sampling immediately after own tail workload",
        uncertainty="previous 20 percent reading is not proven to be external load or self load",
        initial_report_retained=True, total_report_cap=4, extra_formal_pairs=0))
    session = Retest()
    session.preflight()
    backend.write_json(HERE / "resume-power-before.json", power_snapshot())
    try:
        for tag in tags:
            before_idle = session.host_load()
            time.sleep(5)
            begin = session.host_load()
            backend.write_json(HERE / (tag + "-idle.json"), dict(fixed_idle_seconds=5,
                before_idle=before_idle, after_idle=begin))
            if begin["max_load_pct"] > session.cpu_max:
                print("HOST_LOAD_ABOVE_LIMIT_AFTER_FIXED_IDLE", tag, begin["max_load_pct"], flush=True)
                break
            out = HERE / tag
            with session.instance(tag, out) as (process, _):
                rec = session.benchmark(tag, out, process)
                rec["host_before"] = begin
                rec["host_after"] = session.host_load()
                rec["fixed_idle_before_start_seconds"] = 5
                backend.write_json(out / "official-summary.json", rec)
                backend.write_json(out / "tails.json", session.tails(process.pid))
                print("AA_COMPLETE_REMAINING", tag, rec["metrics"]["select1_avg_ms"],
                      rec["rss_benchmark"]["VmHWM"], flush=True)
            if rec["host_after"]["max_load_pct"] > session.cpu_max:
                print("HOST_LOAD_ABOVE_LIMIT_AFTER", tag, rec["host_after"]["max_load_pct"], flush=True)
                break
    finally:
        backend.write_json(HERE / "resume-power-after.json", power_snapshot())
        backend.write_json(HERE / "resume-resource-final.json", session.guard())
    result = analyze()
    result["protocol_note"] = "first report retained; fixed five-second idle before up to three remaining reports"
    backend.write_json(HERE / "aa-result-resumed.json", result)
    print("RESUMED_CALIBRATION_RESULT", json.dumps(result, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except Exception as exc:
        backend.write_json(HERE / "resume-failure.json", dict(error=repr(exc), traceback=traceback.format_exc()))
        raise
