package ru.karolsonchik.aquawidget;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.SystemClock;
import android.widget.RemoteViews;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** Виджет 2x3: температура, уставка, нагрев, свет, кнопки КОРМ и обновить. */
public class AquaWidgetProvider extends AppWidgetProvider {

    public static final String ACTION_REFRESH  = "ru.karolsonchik.aquawidget.REFRESH";
    public static final String ACTION_TICK     = "ru.karolsonchik.aquawidget.TICK";
    public static final String ACTION_FEED     = "ru.karolsonchik.aquawidget.FEED";
    public static final String ACTION_FEEDSTOP = "ru.karolsonchik.aquawidget.FEEDSTOP";

    /** как часто виджет тянет данные, пока телефон не спит */
    private static final long TICK_MS = 5000L;

    /* ---------- жизненный цикл ---------- */

    @Override
    public void onEnabled(Context ctx) {
        super.onEnabled(ctx);
        startTicker(ctx);
    }

    @Override
    public void onDisabled(Context ctx) {
        stopTicker(ctx);
        super.onDisabled(ctx);
    }

    @Override
    public void onUpdate(final Context ctx, AppWidgetManager mgr, int[] ids) {
        for (int id : ids) mgr.updateAppWidget(id, build(ctx));
        startTicker(ctx);
        new Thread(new Runnable() {
            public void run() {
                if (Api.refresh(ctx)) push(ctx);
            }
        }).start();
    }

    @Override
    public void onReceive(final Context ctx, Intent intent) {
        super.onReceive(ctx, intent);
        String a = intent.getAction();
        if (a == null) return;
        if (ACTION_TICK.equals(a) || ACTION_REFRESH.equals(a)
                || Intent.ACTION_BOOT_COMPLETED.equals(a) || Intent.ACTION_USER_PRESENT.equals(a)) {
            startTicker(ctx);
            new Thread(new Runnable() {
                public void run() {
                    if (Api.refresh(ctx)) push(ctx);
                }
            }).start();
        } else if (ACTION_FEED.equals(a)) {
            new Thread(new Runnable() {
                public void run() {
                    Api.feed(ctx, false);
                    if (Api.refresh(ctx)) push(ctx);
                }
            }).start();
        } else if (ACTION_FEEDSTOP.equals(a)) {
            new Thread(new Runnable() {
                public void run() {
                    Api.feed(ctx, true);
                    if (Api.refresh(ctx)) push(ctx);
                }
            }).start();
        }
    }

    /* ---------- будильник на 5 секунд ---------- */

    private static PendingIntent tickIntent(Context ctx) {
        Intent i = new Intent(ctx, AquaWidgetProvider.class).setAction(ACTION_TICK);
        return PendingIntent.getBroadcast(ctx, 7, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    static void startTicker(Context ctx) {
        AlarmManager am = (AlarmManager) ctx.getSystemService(Context.ALARM_SERVICE);
        if (am == null) return;
        try {
            am.setRepeating(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                    SystemClock.elapsedRealtime() + TICK_MS, TICK_MS, tickIntent(ctx));
        } catch (Throwable ignored) { }
    }

    static void stopTicker(Context ctx) {
        AlarmManager am = (AlarmManager) ctx.getSystemService(Context.ALARM_SERVICE);
        if (am == null) return;
        try { am.cancel(tickIntent(ctx)); } catch (Throwable ignored) { }
    }

    /* ---------- отрисовка ---------- */

    static RemoteViews build(Context ctx) {
        SharedPreferences p = Api.prefs(ctx);
        RemoteViews v = new RemoteViews(ctx.getPackageName(), R.layout.widget);

        String host = p.getString(Api.KEY_HOST, "");
        String name = p.getString(Api.KEY_NAME, "Аквариум");
        String temp = p.getString(Api.KEY_TEMP, "");
        String sp = p.getString(Api.KEY_SP, "");
        int heat = p.getInt(Api.KEY_HEAT, 0);
        int light = p.getInt(Api.KEY_LIGHT, 0);
        int al = p.getInt(Api.KEY_ALARM, 0);
        int feed = p.getInt(Api.KEY_FEED, 0);
        int rssi = p.getInt(Api.KEY_RSSI, 0);
        String time = p.getString(Api.KEY_TIME, "");
        long ok = p.getLong(Api.KEY_OK, 0);

        v.setTextViewText(R.id.wName, name != null && name.length() > 0 ? name : "Аквариум");
        v.setTextViewText(R.id.wTemp, temp != null && temp.length() > 0 ? temp : "--");
        v.setTextViewText(R.id.wUnit, "°C");
        v.setTextViewText(R.id.wSp, sp != null && sp.length() > 0 ? "цель " + sp + " °C" : "нет данных");

        int statusColor;
        String status;
        if (host == null || host.length() == 0) {
            status = "не привязан";
            statusColor = Color.parseColor("#FFB020");
        } else if (ok == 0) {
            status = "нет связи";
            statusColor = Color.parseColor("#FF4D5E");
        } else if (feed > 0) {
            status = "кормление";
            statusColor = Color.parseColor("#E9F94B");
        } else if ((al & 0x0D) != 0) {
            status = (al & 0x01) != 0 ? "нет датчика" : "тревога";
            statusColor = Color.parseColor("#FF4D5E");
        } else {
            status = "норма";
            statusColor = Color.parseColor("#22D37F");
        }
        v.setTextViewText(R.id.wStatus, status);
        v.setTextColor(R.id.wStatus, statusColor);

        v.setTextViewText(R.id.wHeat, heat == 1 ? "нагрев ВКЛ" : "нагрев выкл");
        v.setTextColor(R.id.wHeat, heat == 1 ? Color.parseColor("#FFA06A") : Color.parseColor("#8B9BAD"));
        v.setProgressBar(R.id.wLightBar, 100, light, false);
        v.setTextViewText(R.id.wLight, feed > 0
                ? ("помпа на паузе · " + (feed / 60) + " мин " + (feed % 60) + " с")
                : ("свет " + light + "%"));

        /* кнопка КОРМ: во время паузы — отмена */
        v.setTextViewText(R.id.wFeed, feed > 0 ? "ОТМЕНА" : "КОРМ");
        v.setTextColor(R.id.wFeed, feed > 0 ? Color.parseColor("#15200A") : Color.parseColor("#E9F94B"));
        Intent fi = new Intent(ctx, AquaWidgetProvider.class)
                .setAction(feed > 0 ? ACTION_FEEDSTOP : ACTION_FEED);
        PendingIntent fpi = PendingIntent.getBroadcast(ctx, 8, fi,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        v.setOnClickPendingIntent(R.id.wFeed, fpi);

        String foot;
        if (ok > 0) {
            String when = new SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(new Date(ok));
            foot = (rssi != 0 ? rssi + " дБм · " : "") + when + (time.length() > 0 ? " · " + time : "");
        } else {
            foot = (host != null && host.length() > 0) ? host : "нажмите, чтобы настроить";
        }
        v.setTextViewText(R.id.wFoot, foot);

        if (host != null && host.length() > 0) {
            Intent open = new Intent(Intent.ACTION_VIEW, Uri.parse("http://" + host + "/"));
            open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            PendingIntent pi = PendingIntent.getActivity(ctx, 1, open,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            v.setOnClickPendingIntent(R.id.wRoot, pi);
        } else {
            Intent app = new Intent(ctx, MainActivity.class);
            PendingIntent pi = PendingIntent.getActivity(ctx, 2, app,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            v.setOnClickPendingIntent(R.id.wRoot, pi);
        }

        Intent ri = new Intent(ctx, AquaWidgetProvider.class).setAction(ACTION_REFRESH);
        PendingIntent rpi = PendingIntent.getBroadcast(ctx, 3, ri,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        v.setOnClickPendingIntent(R.id.wRefresh, rpi);

        return v;
    }

    static void push(Context ctx) {
        AppWidgetManager m = AppWidgetManager.getInstance(ctx);
        int[] ids = m.getAppWidgetIds(new ComponentName(ctx, AquaWidgetProvider.class));
        for (int id : ids) m.updateAppWidget(id, build(ctx));
    }
}
