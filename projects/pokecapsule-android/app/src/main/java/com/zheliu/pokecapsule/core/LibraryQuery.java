package com.zheliu.pokecapsule.core;

import com.zheliu.pokecapsule.model.CapsuleRecord;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

public final class LibraryQuery {
    private LibraryQuery() {}

    public static List<CapsuleRecord> apply(List<CapsuleRecord> records,
            LibraryScope scope, String search, LibrarySort sort) {
        String query = search == null ? "" : search.trim().toLowerCase(Locale.ROOT);
        ArrayList<CapsuleRecord> result = new ArrayList<>();
        for (CapsuleRecord record : records) {
            if (!scope.includes(record)) continue;
            if (!query.isEmpty() && !searchableText(record).contains(query)) continue;
            result.add(record);
        }
        final int direction = sort == LibrarySort.NEWEST_FIRST ? -1 : 1;
        Collections.sort(result, new Comparator<CapsuleRecord>() {
            @Override public int compare(CapsuleRecord left, CapsuleRecord right) {
                String leftCreated = left.createdAt == null ? "" : left.createdAt;
                String rightCreated = right.createdAt == null ? "" : right.createdAt;
                return direction * leftCreated.compareTo(rightCreated);
            }
        });
        return result;
    }

    private static String searchableText(CapsuleRecord record) {
        StringBuilder text = new StringBuilder();
        append(text, record.title);
        append(text, record.relativeFolder);
        append(text, record.finalText);
        append(text, record.polishedText);
        append(text, record.rawText);
        for (String tag : record.tags) append(text, tag);
        return text.toString().toLowerCase(Locale.ROOT);
    }

    private static void append(StringBuilder destination, String value) {
        if (value == null || value.isEmpty()) return;
        if (destination.length() > 0) destination.append(' ');
        destination.append(value);
    }
}
