package com.zheliu.pokecapsule.core;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Calendar;
import java.util.Locale;
import java.util.TimeZone;

public final class TimeFormat {
    private TimeFormat() {}

    public static String utcNow() {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date());
    }

    public static String localDisplay(String utc) {
        try {
            SimpleDateFormat parser = new SimpleDateFormat(
                    "yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US);
            parser.setTimeZone(TimeZone.getTimeZone("UTC"));
            Date value = parser.parse(utc);
            if (value == null) return utc;
            Calendar now = Calendar.getInstance();
            Calendar date = Calendar.getInstance();
            date.setTime(value);
            SimpleDateFormat time = new SimpleDateFormat("HH:mm", Locale.getDefault());
            if (sameDay(now, date)) return "今天 " + time.format(value);
            now.add(Calendar.DAY_OF_YEAR, -1);
            if (sameDay(now, date)) return "昨天 " + time.format(value);
            return new SimpleDateFormat("M月d日 HH:mm", Locale.getDefault()).format(value);
        } catch (Exception ignored) {
            return utc;
        }
    }

    private static boolean sameDay(Calendar left, Calendar right) {
        return left.get(Calendar.ERA) == right.get(Calendar.ERA)
                && left.get(Calendar.YEAR) == right.get(Calendar.YEAR)
                && left.get(Calendar.DAY_OF_YEAR) == right.get(Calendar.DAY_OF_YEAR);
    }
}
