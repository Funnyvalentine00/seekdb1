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

token_ids = {}

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
            ptr = gdb.parse_and_eval("this")
            token_ids[int(ptr)] = self.seen
            obj = ptr.dereference()
            score = obj["relevance_"]
            doc = obj["doc_id_"]
            item = {
                "init_id": self.seen,
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

class DocLoadProbe(gdb.Breakpoint):
    def __init__(self):
        super().__init__("ob_text_retrieval_token_iter.cpp:534", internal=True)
        self.errors = 0

    def stop(self):
        try:
            ptr = gdb.parse_and_eval("this")
            n = int(ptr.dereference()["count_"])
            if n > 0:
                gdb.write("FTS_DOC_LOAD " + json.dumps({"init_id": token_ids.get(int(ptr), -1), "count": n}) + "\n")
                gdb.flush()
        except (gdb.error, ValueError, TypeError) as exc:
            if self.errors < 3:
                gdb.write("FTS_DOC_ERROR " + str(exc) + "\n")
                gdb.flush()
            self.errors += 1
        return False

observer = ArrayProbe()
doc_observer = DocLoadProbe()
end
run
