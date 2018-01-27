CREATE TABLE nodes(
	id     INTEGER,
	parent INTEGER,
	type   INTEGER NOT NULL DEFAULT 0,
	name   TEXT NOT NULL,
	size   INTEGER NOT NULL DEFAULT 0,
	error  INTEGER NOT NULL DEFAULT 0,
	md5    BLOB,
	sha1   BLOB,
	sha256 BLOB,
	sha512 BLOB,
	PRIMARY KEY(id)
	FOREIGN KEY(parent)
		REFERENCES nodes(id)
	UNIQUE(parent, name)
	CHECK(size >= 0)
	CHECK(name != '')
);

CREATE TRIGGER nodes_after_insert
	AFTER INSERT ON nodes
	WHEN NEW.size != 0
BEGIN
	UPDATE nodes SET
		size = size + NEW.size
	WHERE
		id = NEW.parent;
END;

CREATE TRIGGER nodes_after_delete
	AFTER DELETE ON nodes
	WHEN OLD.size != 0
BEGIN
	UPDATE nodes SET
		size = size - OLD.size
	WHERE
		id = OLD.parent;
END;

CREATE TRIGGER nodes_after_update
	AFTER UPDATE ON nodes
	WHEN OLD.parent = NEW.parent
		AND OLD.size != NEW.size
BEGIN
	UPDATE nodes SET
		size = size - OLD.size + NEW.size
	WHERE
		id = NEW.parent;
END;

CREATE TRIGGER nodes_after_update_old
	AFTER UPDATE ON nodes
	WHEN OLD.parent != NEW.parent
		AND OLD.size != 0
BEGIN
	UPDATE nodes SET
		size = size - OLD.size
	WHERE
		id = OLD.parent;
END;

CREATE TRIGGER nodes_after_update_new
	AFTER UPDATE ON nodes
	WHEN OLD.parent != NEW.parent
		AND NEW.size != 0
BEGIN
	UPDATE nodes SET
		size = size + NEW.size
	WHERE
		id = NEW.parent;
END;
