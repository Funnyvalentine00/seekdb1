set confirm off
set pagination off
set auto-load off
set debuginfod enabled off
set startup-with-shell off
set disable-randomization off
set print thread-events off
handle SIGPIPE nostop noprint pass
handle SIGUSR1 nostop noprint pass
python
import gdb
import json

class ArrayProbe(gdb.Breakpoint):
    def __init__(self):
        super().__init__("ob_text_retrieval_token_iter.cpp:454", internal=True)
        self.errors = 0
        self.seen = 0

    def stop(self):
        if self.seen >= 15000:
            return False
        self.seen += 1
        try:
            obj = gdb.parse_and_eval("this").dereference()
            score = obj["relevance_"]
            doc = obj["doc_id_"]
            item = {
                "scoreless": int(obj["relevance_expr_"]) == 0,
                "batch": int(obj["max_batch_size_"]),
                "score_capacity": int(score["capacity_"]),
                "score_count": int(score["count_"]),
                "score_allocated": int(score["data_"]) != 0,
                "score_item_bytes": score["data_"].type.target().sizeof,
                "doc_capacity": int(doc["capacity_"]),
                "doc_count": int(doc["count_"]),
                "doc_allocated": int(doc["data_"]) != 0,
                "doc_item_bytes": doc["data_"].type.target().sizeof,
            }
            gdb.write("FTS_ARRAY " + json.dumps(item, sort_keys=True) + "\n")
        except (gdb.error, ValueError, TypeError) as exc:
            if self.errors < 3:
                gdb.write("FTS_ARRAY_ERROR " + str(exc) + "\n")
            self.errors += 1
        gdb.flush()
        return False

observer = ArrayProbe()
end
run
