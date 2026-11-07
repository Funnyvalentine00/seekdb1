"""Execute the corrected basic_dml fixture against the original golden."""
import signal

from run_cases import HERE, Retest, backend, interrupted, run_case


def main():
    assert (HERE / "red.rc").read_text().strip() == "0"
    session = Retest()
    session.preflight()
    out = HERE / "shadow-red"
    official = session.src / "tools/deploy/mysql_test/test_suite/fts_index"
    with session.instance("shadow-red", out) as (_, data):
        record = run_case(session, data, out, "basic_dml", HERE / "shadow/basic_dml.test",
                          official / "r/mysql/basic_dml.result")
        backend.write_json(out / "case-summary.json", [record])


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    main()
