# MATCH query path vs. the tokenize fast path

Date: 2026-11-05
Question: does `MATCH ... AGAINST ...` query initialization use the new
empty-property fast path in `ObFTParseHelper::init`?

## Answer

No for the benchmark indexes. The fast path is only taken when the parser
property string is empty; the benchmark FULLTEXT indexes store non-empty
`parser_properties`, so MATCH goes through the original
`parse_for_parser_helper` branch.

## Code path

1. `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp:1107-1116`
   (and the equivalent `src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp:237-243`)
   obtains the parser name and property string from the index scan definition:

   ```cpp
   const ObString &parser_name =
       ir_ctdef->get_inv_idx_scan_ctdef()->table_param_.get_parser_name();
   const ObString &parser_properties =
       ir_ctdef->get_inv_idx_scan_ctdef()->table_param_.get_parser_property();
   ...
   tokenize_helper.init(&alloc, parser_name, parser_properties);
   ```

2. `src/storage/fts/ob_fts_parser_helper.cpp:358-361` selects the branch:

   ```cpp
   } else if (!parser_properties.empty()
              && OB_FAIL(parser_property_.parse_for_parser_helper(parser_name_, parser_properties))) {
   } else if (parser_properties.empty()
              && FALSE_IT(set_default_parser_property(parser_name_, parser_property_))) {
   ```

   The `set_default_parser_property` fast path is therefore unreachable for a
   non-empty property string.

## Catalog evidence

On the benchmark data directory (2026-11-05, live instance), the FULLTEXT
index aux tables carry non-empty properties:

```text
table_name                                 parser_name  parser_properties
__idx_500003_fti_all_ik_fts_doc_word       ik.1         {"ik_mode": "smart", "dict_table": "oceanbase.__ft_dict_ik_utf8", "quanitfier_table": "oceanbase.__ft_quantifier_ik_utf8"}
__idx_500003_fti_content_ik_fts_doc_word   ik.1         {"ik_mode": "smart", "dict_table": "oceanbase.__ft_dict_ik_utf8", "quanitfier_table": "oceanbase.__ft_quantifier_ik_utf8"}
__idx_500003_fti_en_beng_fts_doc_word      beng.1       {"max_token_size": 84, "min_token_size": 3}
__idx_500935_ctx_fts_doc_word              space.1      {"max_token_size": 84, "min_token_size": 3}
```

The base `docs` table itself has NULL parser properties, but MATCH reads the
index aux table's properties, not the base table's. Inspecting only `docs`
would be misleading.

## Implication

- The v4 platform query gains (`query_cn/beng/mixed/limit +7%~13%`) cannot be
  attributed to this fast path for the benchmark workload. The comparator fast
  paths (`ObSRMergeCmp` and `ObTextRetrievalDaaTTokenIter::advance_to`) remain
  the query-side explanation, consistent with their instruction-count and
  wall-clock A/B results.
- The tokenize fast path remains valid for `tokenize(text, parser)` calls where
  the property string is empty; those are covered by the 21-case byte-identical
  diff and the platform's `tokenize_ik` result.
- For future query work, do not spend time on `ObFTParseHelper` initialization
  for this benchmark; target the comparator/merge loops instead.
