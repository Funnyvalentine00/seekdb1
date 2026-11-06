from pathlib import Path
import hashlib,json,sys
root=Path(__file__).resolve().parent
variant=sys.argv[1]
manifest=json.loads((root/'source-manifest.json').read_text())[variant]
actual={p:hashlib.sha256((root/'src'/p).read_bytes()).hexdigest() for p in manifest}
assert actual==manifest, [(p,manifest[p],actual[p]) for p in manifest if manifest[p]!=actual[p]]
(root/'logs'/('source-'+variant+'.json')).write_text(json.dumps(actual,indent=2))
print('SOURCE_MATCH',variant,len(actual))
