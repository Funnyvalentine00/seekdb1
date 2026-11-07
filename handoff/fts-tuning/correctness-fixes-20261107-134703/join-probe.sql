SET NAMES utf8mb4;
CREATE DATABASE fts_defect_join_134703;
USE fts_defect_join_134703;
create table t_ir_single_column(
  seq int PRIMARY KEY,
  digest varchar(512),
  detail varchar(4096),
  fulltext index i1(detail)) partition by hash(seq) partitions 2;

create table t_dim_table(
    seq int,
    log varchar(1024));

insert into t_ir_single_column (seq, digest, detail) values
    (0, "Gerard Salton", "Gerard A. Gerry Salton (8 March 1927 – 28 August 1995) was a professor of Computer Science at Cornell University. Salton was perhaps the leading computer scientist working in the field of information retrieval during his time, and the father of Information Retrieval"),
    (1, "Edgar F. Codd", "Edgar Frank Ted Codd (19 August 1923 – 18 April 2003) was an English computer scientist who, while working for IBM, invented the relational model for database management, the theoretical basis for relational databases and relational database management systems. "),
    (2, "Michael Stonebraker", "Michael Ralph Stonebraker (born October 11, 1943[6]) is a computer scientist specializing in database systems. Through a series of academic prototypes and commercial startups, Stonebraker's research and products are central to many relational databases. "),
    (3, "Geoffrey Hinton", "Geoffrey Everest Hinton CC FRS FRSC (born 6 December 1947) is a British-Canadian computer scientist and cognitive psychologist, most noted for his work on artificial neural networks."),
    (4, "Yann LeCun", "Yann André LeCun[1] (born 8 July 1960) is a Turing Award winning French computer scientist working primarily in the fields of machine learning, computer vision, mobile robotics and computational neuroscience."),
    (5, "Ken Thompson", "Kenneth Lane Thompson (born February 4, 1943) is an American pioneer of computer science. Thompson worked at Bell Labs for most of his career where he designed and implemented the original Unix operating system. "),
    (6, "Gordon Plotkin", "Gordon David Plotkin, FRS FRSE MAE (born 9 September 1946) is a theoretical computer scientist in the School of Informatics at the University of Edinburgh. Plotkin is probably best known for his introduction of structural operational semantics (SOS) and his work on denotational semantics."),
    (7, "Leslie Lamport", "Leslie B. Lamport (born February 7, 1941) is an American computer scientist and mathematician. Lamport is best known for his seminal work in distributed systems, and as the initial developer of the document preparation system LaTeX");

insert into t_dim_table (seq, log) values
    (1, "Codd log"),
    (3, "Hinton log"),
    (5, "Ken log"),
    (7, "Lamport log"),
    (9, "someone's log");


SET ob_enable_index_direct_select = 'ON';
SELECT '---plan---';
explain
select /*+ use_das(t_ir_single_column) */
    t_ir_single_column.digest, t_ir_single_column.seq, t_dim_table.seq, t_dim_table.log as log , match (detail) against ("computer") as score
from
    t_ir_single_column, t_dim_table
where
    t_ir_single_column.seq < t_dim_table.seq and match (detail) against ("computer")
order by
    t_ir_single_column.seq, t_dim_table.seq;
SELECT '---actual---';
select /*+ use_das(t_ir_single_column) */
    t_ir_single_column.digest, t_ir_single_column.seq, t_dim_table.seq, t_dim_table.log as log , match (detail) against ("computer") as score
from
    t_ir_single_column, t_dim_table
where
    t_ir_single_column.seq < t_dim_table.seq and match (detail) against ("computer")
order by
    t_ir_single_column.seq, t_dim_table.seq;
SELECT '---match-rows---';
SELECT digest, seq, MATCH(detail) AGAINST('computer') FROM t_ir_single_column
  WHERE MATCH(detail) AGAINST('computer') ORDER BY seq;
SELECT '---dim-rows---';
SELECT seq, log FROM t_dim_table ORDER BY seq;
