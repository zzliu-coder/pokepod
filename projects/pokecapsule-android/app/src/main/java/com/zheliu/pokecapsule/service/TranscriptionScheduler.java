package com.zheliu.pokecapsule.service;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.os.PersistableBundle;

public final class TranscriptionScheduler {
    private static final int AUTO_JOB_ID = 3401;
    private static final int MANUAL_JOB_ID = 3402;

    private TranscriptionScheduler() {}

    public static void scheduleAutomatic(Context context) {
        schedule(context, AUTO_JOB_ID, true, false);
    }

    public static void scheduleManual(Context context) {
        schedule(context, MANUAL_JOB_ID, false, true);
    }

    private static void schedule(Context context, int id, boolean charging, boolean manual) {
        JobScheduler scheduler = (JobScheduler) context.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (scheduler == null) return;
        PersistableBundle extras = new PersistableBundle();
        extras.putBoolean("manual", manual);
        JobInfo.Builder builder = new JobInfo.Builder(
                id, new ComponentName(context, TranscriptionJobService.class))
                .setPersisted(true)
                .setRequiresCharging(charging)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_UNMETERED)
                .setRequiresDeviceIdle(false)
                .setBackoffCriteria(30_000L, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .setExtras(extras);
        scheduler.schedule(builder.build());
    }
}
