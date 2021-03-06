CREATE TABLE nodes(
	id     INTEGER,
	parent INTEGER,
	type   INTEGER NOT NULL DEFAULT 0,
	name   TEXT NOT NULL,
	size   INTEGER NOT NULL DEFAULT 0,
	atime  INTEGER NOT NULL DEFAULT 0,
	mtime  INTEGER NOT NULL DEFAULT 0,
	ctime  INTEGER NOT NULL DEFAULT 0,
	error  INTEGER NOT NULL DEFAULT 0,
	md5    BLOB,
	sha1   BLOB,
	sha256 BLOB,
	sha512 BLOB,
	PRIMARY KEY(id),
	FOREIGN KEY(parent)
		REFERENCES nodes(id),
	UNIQUE(parent, name),
	CHECK(name <> ''),
	CHECK(size >= 0)
);

CREATE TRIGGER nodes_insert
	AFTER INSERT ON nodes
	WHEN NEW.size <> 0
BEGIN
	UPDATE nodes SET
		size = size + NEW.size
	WHERE
		id = NEW.parent;
END;

CREATE TRIGGER nodes_delete
	AFTER DELETE ON nodes
	WHEN OLD.size <> 0
BEGIN
	UPDATE nodes SET
		size = size - OLD.size
	WHERE
		id = OLD.parent;
END;

CREATE TRIGGER nodes_update
	AFTER UPDATE ON nodes
	WHEN OLD.parent IS NEW.parent
		AND OLD.size <> NEW.size
BEGIN
	UPDATE nodes SET
		size = size - OLD.size + NEW.size
	WHERE
		id = NEW.parent;
END;

CREATE TRIGGER nodes_update_old
	AFTER UPDATE ON nodes
	WHEN OLD.parent IS NOT NEW.parent
		AND OLD.size <> 0
BEGIN
	UPDATE nodes SET
		size = size - OLD.size
	WHERE
		id = OLD.parent;
END;

CREATE TRIGGER nodes_update_new
	AFTER UPDATE ON nodes
	WHEN OLD.parent IS NOT NEW.parent
		AND NEW.size <> 0
BEGIN
	UPDATE nodes SET
		size = size + NEW.size
	WHERE
		id = NEW.parent;
END;
