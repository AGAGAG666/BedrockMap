package io.github.hhhxiao.bedrockmap;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

public final class McpKeepAliveService extends Service {
    private static final String TAG = "BedrockMapMcp";
    private static final String CHANNEL = "mcp_service";
    private static final String STOP = "io.github.hhhxiao.bedrockmap.STOP_MCP";
    private static final String GENERATION = "generation";
    private static final int NOTIFICATION_ID = 8765;
    private static boolean askedForNotifications;

    private long generation;
    private PowerManager.WakeLock wakeLock;

    private static native void notifyStopped(long generation, boolean failed);

    public static boolean start(Context context, long generation) {
        Intent intent = new Intent(context, McpKeepAliveService.class).putExtra(GENERATION, generation);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(intent);
            else context.startService(intent);
        } catch (RuntimeException error) {
            AppLog.w(TAG, "Could not start the MCP service.", error);
            return false;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && context instanceof Activity) {
            Activity activity = (Activity) context;
            activity.runOnUiThread(() -> {
                if (askedForNotifications || activity.isFinishing() || activity.isDestroyed() ||
                        activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED)
                    return;
                askedForNotifications = true;
                activity.requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, NOTIFICATION_ID);
            });
        }
        return true;
    }

    public static void stop(Context context) {
        context.stopService(new Intent(context, McpKeepAliveService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        long requestedGeneration = intent.getLongExtra(GENERATION, 0);
        if (STOP.equals(intent.getAction())) {
            if (generation == 0 || generation == requestedGeneration) stopSelf(startId);
            return START_NOT_STICKY;
        }
        generation = requestedGeneration;
        try {
            Notification notification = buildNotification();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else startForeground(NOTIFICATION_ID, notification);
            if (wakeLock == null) {
                PowerManager power = getSystemService(PowerManager.class);
                wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "BedrockMap:MCP");
                wakeLock.acquire();
            }
        } catch (RuntimeException error) {
            AppLog.w(TAG, "Could not keep the MCP service running.", error);
            if (generation != 0) notifyStopped(generation, true);
            generation = 0;
            stopSelf(startId);
        }
        return START_NOT_STICKY;
    }

    @SuppressWarnings("deprecation")
    private Notification buildNotification() {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(CHANNEL, getString(R.string.mcp_service_title),
                    NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
            builder = new Notification.Builder(this, CHANNEL);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        } else {
            builder = new Notification.Builder(this).setPriority(Notification.PRIORITY_LOW);
        }
        Intent stopIntent = new Intent(this, McpKeepAliveService.class).setAction(STOP).putExtra(GENERATION, generation);
        PendingIntent stopAction = PendingIntent.getService(this, 0, stopIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return builder.setSmallIcon(R.drawable.ic_mcp_notification)
                .setContentTitle(getString(R.string.mcp_service_title))
                .setContentText(getString(R.string.mcp_service_running))
                .setOngoing(true)
                .addAction(new Notification.Action.Builder(null, getString(R.string.mcp_service_stop), stopAction).build())
                .build();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onDestroy() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        stopForeground(true);
        if (generation != 0) notifyStopped(generation, false);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
