package com.zheliu.pokecapsule.service;

import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.os.PersistableBundle;

public final class TranscriptionScheduler {
    private static final int AUTO_JOB_ID = 3401;
    private static final int AUTO_FOLLOW_UP_JOB_ID = 3403;
    private static final int MANUAL_JOB_ID = 3402;

    private TranscriptionScheduler() {}

    public static void scheduleAutomatic(Context context) {
        JobScheduler scheduler =
                (JobScheduler) context.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (scheduler == null) return;
        int id = !hasJob(scheduler, AUTO_JOB_ID)
                ? AUTO_JOB_ID
                : !hasJob(scheduler, AUTO_FOLLOW_UP_JOB_ID)
                        ? AUTO_FOLLOW_UP_JOB_ID
                        : -1;
        if (id >= 0) schedule(context, scheduler, id, false);
    }

    public static void scheduleManual(Context context) {
        JobScheduler scheduler =
                (JobScheduler) context.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (scheduler == null || hasJob(scheduler, MANUAL_JOB_ID)) return;
        schedule(context, scheduler, MANUAL_JOB_ID, true);
    }

    public static void cancelAutomatic(Context context) {
        JobScheduler scheduler =
                (JobScheduler) context.getSystemService(Context.JOB_SCHEDULER_SERVICE);
        if (scheduler != null) {
            scheduler.cancel(AUTO_JOB_ID);
            scheduler.cancel(AUTO_FOLLOW_UP_JOB_ID);
        }
    }

    private static void schedule(
            Context context, JobScheduler scheduler, int id, boolean manual) {
        PersistableBundle extras = new PersistableBundle();
        extras.putBoolean("manual", manual);
        JobInfo.Builder builder = new JobInfo.Builder(
                id, new ComponentName(context, TranscriptionJobService.class))
                .setPersisted(false)
                .setRequiresCharging(false)
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setRequiresDeviceIdle(false)
                .setBackoffCriteria(30_000L, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .setExtras(extras);
        scheduler.schedule(builder.build());
    }

    private static boolean hasJob(JobScheduler scheduler, int id) {
        for (JobInfo pending : scheduler.getAllPendingJobs()) {
            if (pending.getId() == id) return true;
        }
        return false;
    }
}
