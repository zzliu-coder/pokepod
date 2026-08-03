package com.zheliu.pokecapsule.core;

import java.util.Locale;
import java.util.UUID;

public final class Ids {
    private Ids() {}

    public static String newId() {
        return UUID.randomUUID().toString().toLowerCase(Locale.ROOT);
    }

    public static String newCopyId(String sourceId) {
        if (!isUuid(sourceId)) throw new IllegalArgumentException("sourceId must be UUID");
        String result;
        do {
            result = newId();
        } while (result.equalsIgnoreCase(sourceId));
        return result;
    }

    public static boolean isUuid(String value) {
        if (value == null || value.length() != 36) return false;
        try {
            return UUID.fromString(value).toString().equalsIgnoreCase(value);
        } catch (IllegalArgumentException error) {
            return false;
        }
    }

    public static String normalized(String value) {
        if (!isUuid(value)) throw new IllegalArgumentException("value must be UUID");
        return UUID.fromString(value).toString().toLowerCase(Locale.ROOT);
    }
}
