package com.zheliu.pokecapsule.storage;

import com.zheliu.pokecapsule.model.CapsuleRecord;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Lightweight in-memory search suited to the Poke3's small local library. */
public final class SearchIndex {
    private SearchIndex() {}

    public static List<CapsuleRecord> filter(List<CapsuleRecord> source, String input) {
        String query = normalize(input);
        if (query.isEmpty()) return new ArrayList<>(source);
        ArrayList<CapsuleRecord> result = new ArrayList<>();
        for (CapsuleRecord record : source) {
            if (haystack(record).contains(query)) result.add(record);
        }
        return result;
    }

    private static String haystack(CapsuleRecord record) {
        StringBuilder value = new StringBuilder();
        value.append(record.title).append('\n')
                .append(record.finalText).append('\n')
                .append(record.polishedText).append('\n')
                .append(record.rawText).append('\n')
                .append(record.relativeFolder).append('\n');
        for (String tag : record.tags) value.append(tag).append('\n');
        return normalize(value.toString());
    }

    private static String normalize(String value) {
        return value == null ? "" : value.trim().toLowerCase(Locale.ROOT);
    }
}
