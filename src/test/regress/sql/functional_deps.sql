-- from http://www.depesz.com/index.php/2010/04/19/getting-unique-elements/

CREATE TABLE articles (
    id int CONSTRAINT articles_pkey PRIMARY KEY,
    keywords text,
    title text UNIQUE,
    body text UNIQUE,
    created date
);

CREATE TABLE articles_in_category (
    article_id int,
    category_id int,
    changed date,
    PRIMARY KEY (article_id, category_id)
);

-- test functional dependencies based on primary keys/unique constraints

-- base tables

-- group by primary key (OK)
SELECT id, keywords, title, body, created
FROM articles
GROUP BY id;

-- minipg: NOT NULL 约束已裁剪，title 只是 nullable UNIQUE，
-- 不构成函数依赖，此用例改为预期失败
SELECT id, keywords, title, body, created
FROM articles
GROUP BY title;

-- group by unique nullable (fail)
SELECT id, keywords, title, body, created
FROM articles
GROUP BY body;

-- group by something else (fail)
SELECT id, keywords, title, body, created
FROM articles
GROUP BY keywords;

-- multiple tables

-- group by primary key (OK)
SELECT a.id, a.keywords, a.title, a.body, a.created
FROM articles AS a, articles_in_category AS aic
WHERE a.id = aic.article_id AND aic.category_id in (14,62,70,53,138)
GROUP BY a.id;

-- group by something else (fail)
SELECT a.id, a.keywords, a.title, a.body, a.created
FROM articles AS a, articles_in_category AS aic
WHERE a.id = aic.article_id AND aic.category_id in (14,62,70,53,138)
GROUP BY aic.article_id, aic.category_id;

-- JOIN syntax

-- group by left table's primary key (OK)
SELECT a.id, a.keywords, a.title, a.body, a.created
FROM articles AS a JOIN articles_in_category AS aic ON a.id = aic.article_id
WHERE aic.category_id in (14,62,70,53,138)
GROUP BY a.id;

-- group by something else (fail)
SELECT a.id, a.keywords, a.title, a.body, a.created
FROM articles AS a JOIN articles_in_category AS aic ON a.id = aic.article_id
WHERE aic.category_id in (14,62,70,53,138)
GROUP BY aic.article_id, aic.category_id;

-- group by right table's (composite) primary key (OK)
SELECT aic.changed
FROM articles AS a JOIN articles_in_category AS aic ON a.id = aic.article_id
WHERE aic.category_id in (14,62,70,53,138)
GROUP BY aic.category_id, aic.article_id;

-- group by right table's partial primary key (fail)
SELECT aic.changed
FROM articles AS a JOIN articles_in_category AS aic ON a.id = aic.article_id
WHERE aic.category_id in (14,62,70,53,138)
GROUP BY aic.article_id;


-- example from documentation

-- minipg: numeric 已裁剪，改用 int
CREATE TABLE products (product_id int, name text, price int);
CREATE TABLE sales (product_id int, units int);

-- OK
SELECT product_id, p.name, (sum(s.units) * p.price) AS sales
    FROM products p LEFT JOIN sales s USING (product_id)
    GROUP BY product_id, p.name, p.price;

-- fail
SELECT product_id, p.name, (sum(s.units) * p.price) AS sales
    FROM products p LEFT JOIN sales s USING (product_id)
    GROUP BY product_id;

ALTER TABLE products ADD PRIMARY KEY (product_id);

-- OK now
SELECT product_id, p.name, (sum(s.units) * p.price) AS sales
    FROM products p LEFT JOIN sales s USING (product_id)
    GROUP BY product_id;


-- Drupal example, http://drupal.org/node/555530

-- minipg: SERIAL、NOT NULL 与列 DEFAULT 均已裁剪
CREATE TABLE node (
    nid int,
    vid integer,
    type varchar(32),
    title varchar(128),
    uid integer,
    status integer,
    created integer,
    -- snip
    PRIMARY KEY (nid, vid)
);

CREATE TABLE users (
    uid integer,
    name varchar(60),
    pass varchar(32),
    -- snip
    PRIMARY KEY (uid),
    UNIQUE (name)
);

-- OK
SELECT u.uid, u.name FROM node n
INNER JOIN users u ON u.uid = n.uid
WHERE n.type = 'blog' AND n.status = 1
GROUP BY u.uid, u.name;

-- OK
SELECT u.uid, u.name FROM node n
INNER JOIN users u ON u.uid = n.uid
WHERE n.type = 'blog' AND n.status = 1
GROUP BY u.uid;


-- Check views and dependencies

-- fail
CREATE VIEW fdv1 AS
SELECT id, keywords, title, body, created
FROM articles
GROUP BY body;

-- OK
CREATE VIEW fdv1 AS
SELECT id, keywords, title, body, created
FROM articles
GROUP BY id;

-- fail
ALTER TABLE articles DROP CONSTRAINT articles_pkey RESTRICT;

DROP VIEW fdv1;


-- multiple dependencies
CREATE VIEW fdv2 AS
SELECT a.id, a.keywords, a.title, aic.category_id, aic.changed
FROM articles AS a JOIN articles_in_category AS aic ON a.id = aic.article_id
WHERE aic.category_id in (14,62,70,53,138)
GROUP BY a.id, aic.category_id, aic.article_id;

ALTER TABLE articles DROP CONSTRAINT articles_pkey RESTRICT; -- fail
ALTER TABLE articles_in_category DROP CONSTRAINT articles_in_category_pkey RESTRICT; --fail

DROP VIEW fdv2;


-- nested queries

CREATE VIEW fdv3 AS
SELECT id, keywords, title, body, created
FROM articles
GROUP BY id;

ALTER TABLE articles DROP CONSTRAINT articles_pkey RESTRICT; -- fail

DROP VIEW fdv3;


CREATE VIEW fdv4 AS
SELECT * FROM articles WHERE title IN (SELECT title FROM articles GROUP BY id);

ALTER TABLE articles DROP CONSTRAINT articles_pkey RESTRICT; -- fail

DROP VIEW fdv4;


