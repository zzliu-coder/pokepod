package com.zheliu.pokecapsule.ui;

import com.zheliu.pokecapsule.core.LibraryQuery;
import com.zheliu.pokecapsule.core.LibraryScope;
import com.zheliu.pokecapsule.core.LibrarySort;
import com.zheliu.pokecapsule.model.CapsuleRecord;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Owns library navigation, search, sorting and selection independent of Activities. */
final class LibraryController {
    private LibraryScope scope = LibraryScope.INBOX;
    private LibrarySort sort = LibrarySort.NEWEST_FIRST;
    private String search = "";
    private final Set<String> selection = new HashSet<>();

    List<CapsuleRecord> query(List<CapsuleRecord> records) {
        return LibraryQuery.apply(records, scope, search, sort);
    }

    Set<String> selection() { return selection; }
    LibraryScope scope() { return scope; }
    boolean isTrash() { return scope.kind == LibraryScope.Kind.TRASH; }
    String search() { return search; }
    LibrarySort sort() { return sort; }

    void show(LibraryScope nextScope) {
        scope = nextScope;
        search = "";
        selection.clear();
    }

    void search(String query) {
        scope = LibraryScope.ALL;
        search = query == null ? "" : query.trim();
        selection.clear();
    }

    void toggleSort() {
        sort = sort == LibrarySort.NEWEST_FIRST
                ? LibrarySort.OLDEST_FIRST : LibrarySort.NEWEST_FIRST;
    }

    void toggleSelection(String id) {
        if (!selection.add(id)) selection.remove(id);
    }
}
