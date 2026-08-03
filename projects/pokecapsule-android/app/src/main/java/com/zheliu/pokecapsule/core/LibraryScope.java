package com.zheliu.pokecapsule.core;

import com.zheliu.pokecapsule.model.CapsuleRecord;

import java.util.Objects;

/** A derived library view. Scope values are never persisted in the protocol. */
public final class LibraryScope {
    public enum Kind { ALL, INBOX, FAVORITES, PENDING, FAILED, FOLDER, TAG, TRASH }

    public static final LibraryScope ALL = new LibraryScope(Kind.ALL, "");
    public static final LibraryScope INBOX = new LibraryScope(Kind.INBOX, PathPolicy.INBOX);
    public static final LibraryScope FAVORITES = new LibraryScope(Kind.FAVORITES, "");
    public static final LibraryScope PENDING = new LibraryScope(Kind.PENDING, "");
    public static final LibraryScope FAILED = new LibraryScope(Kind.FAILED, "");
    public static final LibraryScope TRASH = new LibraryScope(Kind.TRASH, "");

    public final Kind kind;
    public final String value;

    private LibraryScope(Kind kind, String value) {
        this.kind = kind;
        this.value = value == null ? "" : value;
    }

    public static LibraryScope folder(String path) {
        String normalized = PathPolicy.normalizeRelativeFolder(path);
        return PathPolicy.INBOX.equals(normalized) ? INBOX : new LibraryScope(Kind.FOLDER, normalized);
    }

    public static LibraryScope tag(String tag) {
        return new LibraryScope(Kind.TAG, tag == null ? "" : tag.trim());
    }

    public boolean includes(CapsuleRecord record) {
        switch (kind) {
            case ALL: return !record.trashed;
            case INBOX: return !record.trashed && PathPolicy.INBOX.equals(record.relativeFolder);
            case FAVORITES: return !record.trashed && record.favorite;
            case PENDING:
                return !record.trashed && (record.status == ProcessingState.RECORDED
                        || record.status == ProcessingState.QUEUED
                        || record.status == ProcessingState.TRANSCRIBING);
            case FAILED: return !record.trashed && record.status == ProcessingState.FAILED;
            case FOLDER: return !record.trashed && value.equals(record.relativeFolder);
            case TAG: return !record.trashed && record.tags.contains(value);
            case TRASH: return record.trashed;
            default: return false;
        }
    }

    @Override public boolean equals(Object other) {
        if (!(other instanceof LibraryScope)) return false;
        LibraryScope scope = (LibraryScope) other;
        return kind == scope.kind && value.equals(scope.value);
    }

    @Override public int hashCode() { return Objects.hash(kind, value); }
}
