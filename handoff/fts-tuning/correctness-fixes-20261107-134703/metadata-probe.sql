SET NAMES utf8mb4;
CREATE DATABASE fts_defect_meta_134703;
USE fts_defect_meta_134703;
SET @db_id = (SELECT database_id FROM oceanbase.__all_database WHERE database_name = 'fts_defect_meta_134703');
CREATE TABLE same_name (
  id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  title VARCHAR(200), content TEXT, FULLTEXT INDEX ctx(title, content)
);
SET @old_id = (SELECT table_id FROM oceanbase.__all_table WHERE database_id = @db_id AND table_name = 'same_name');
SELECT 'explicit_aux', index_type, parser_name FROM oceanbase.__all_table
  WHERE data_table_id = @old_id AND index_type BETWEEN 11 AND 14 ORDER BY index_type;
DROP TABLE same_name;
CREATE TABLE same_name (title VARCHAR(200), content TEXT, FULLTEXT INDEX ctx(title, content));
SET @new_id = (SELECT table_id FROM oceanbase.__all_table WHERE database_id = @db_id AND table_name = 'same_name');
SELECT 'recreated_ids', @old_id, @new_id, @old_id <> @new_id;
SELECT 'stale_id_aux_count', COUNT(*) FROM oceanbase.__all_table
  WHERE data_table_id = @old_id AND index_type BETWEEN 11 AND 14;
SELECT 'hidden_base_columns', column_name, is_hidden FROM oceanbase.__all_column
  WHERE table_id = @new_id ORDER BY column_id;
SELECT 'hidden_aux', index_type, parser_name FROM oceanbase.__all_table
  WHERE data_table_id = @new_id AND index_type BETWEEN 11 AND 14 ORDER BY index_type;
SELECT 'hidden_aux_columns', t.index_type, c.column_name, c.is_hidden
  FROM oceanbase.__all_table t JOIN oceanbase.__all_column c ON t.table_id = c.table_id
  WHERE t.data_table_id = @new_id AND t.index_type BETWEEN 11 AND 14
  ORDER BY t.index_type, c.column_id;
INSERT INTO same_name(title, content) VALUES ('alpha', 'fixtureneedle'), ('beta', 'fixtureneedle');
SELECT 'after_insert', GROUP_CONCAT(title ORDER BY title) FROM same_name
  WHERE MATCH(title, content) AGAINST ('fixtureneedle' IN BOOLEAN MODE);
UPDATE same_name SET content = 'fixtureother' WHERE title = 'alpha';
SELECT 'after_update_old_term', GROUP_CONCAT(title ORDER BY title) FROM same_name
  WHERE MATCH(title, content) AGAINST ('fixtureneedle' IN BOOLEAN MODE);
SELECT 'after_update_new_term', GROUP_CONCAT(title ORDER BY title) FROM same_name
  WHERE MATCH(title, content) AGAINST ('fixtureother' IN BOOLEAN MODE);
DELETE FROM same_name WHERE title = 'beta';
SELECT 'after_delete', COUNT(*) FROM same_name
  WHERE MATCH(title, content) AGAINST ('fixtureneedle' IN BOOLEAN MODE);
