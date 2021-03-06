/* contrib/fuzzystrmatch/fuzzystrmatch--unpackaged--1.0.sql */

ALTER EXTENSION fuzzystrmatch ADD function levenshtein(text,text);
ALTER EXTENSION fuzzystrmatch ADD function levenshtein(text,text,integer,integer,integer);
ALTER EXTENSION fuzzystrmatch ADD function metaphone(text,integer);
ALTER EXTENSION fuzzystrmatch ADD function soundex(text);
ALTER EXTENSION fuzzystrmatch ADD function text_soundex(text);
ALTER EXTENSION fuzzystrmatch ADD function difference(text,text);
ALTER EXTENSION fuzzystrmatch ADD function dmetaphone(text);
ALTER EXTENSION fuzzystrmatch ADD function dmetaphone_alt(text);

-- these functions were not in 9.0

CREATE FUNCTION levenshtein_less_equal (text,text,int) RETURNS int
AS 'MODULE_PATHNAME','levenshtein_less_equal'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION levenshtein_less_equal (text,text,int,int,int,int) RETURNS int
AS 'MODULE_PATHNAME','levenshtein_less_equal_with_costs'
LANGUAGE C IMMUTABLE STRICT;
