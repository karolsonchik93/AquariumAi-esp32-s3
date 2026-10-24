package ru.karolsonchik.aquawidget;

import android.app.PendingIntent;
import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.widget.RemoteViews;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** Виджет 2x3 на главный экран: температура, уставка, нагрев, свет, связь. */
public class AquaWidgetProvider extends AppWidgetProvider {

    public static final String ACTION_REFRESH = "ru.karolsonchik.aquawidget.REFRESH";

    @Override
    public void onUpdate(final Context ctx, AppWidgetManager mgr, int[] ids) {
        for (int id : ids) mgr.updateAppWidget(id, build(ctx));
        final PendingResult pr = goAsync();
        new Thread(new Runnable() {
            public void run() {
                try {
                    if (Api.refresh(ctx)) {
                        AppWidgetManager m = AppWidgetManager.getInstance(ctx);
                        int[] all = m.getAppWidgetIds(new ComponentName(ctx, AquaWidgetProvider.class));
                        for (int id : all) m.updateAppWidget(id, build(ctx));
                    }
                } finally {
                    pr.finish();
                }
            }
        }).start();
    }

    @Override
    public void onReceive(Context ctx, Intent intent) {
        super.onReceive(ctx, intent);
        if (ACTION_REFRESH.equals(intent.getAction())) {
            AppWidgetManager m = AppWidgetManager.getInstance(ctx);
            int[] ids = m.getAppWidgetIds(new ComponentName(ctx, AquaWidgetProvider.class));
            onUpdate(ctx, m, ids);
        }
    }

    /** Текст виджета из кэша; сеть дёргается отдельно. */
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
        int rssi = p.getInt(Api.KEY_RSSI, 0);
        String time = p.getString(Api.KEY_TIME, "");
        long ok = p.getLong(Api.KEY_OK, 0);

        v.setTextViewText(R.id.wName, name.length() > 0 ? name : "Аквариум");
        v.setTextViewText(R.id.wTemp, temp.length() > 0 ? temp : "--");
        v.setTextViewText(R.id.wUnit, "°C");
        v.setTextViewText(R.id.wSp, sp.length() > 0 ? "цель " + sp + " °C" : "нет данных");

        int statusColor;
        String status;
        if (host.length() == 0) {
            status = "не привязан";
            statusColor = Color.parseColor("#FFB020");
        } else if (ok == 0) {
            status = "нет связи";
            statusColor = Color.parseColor("#FF4D5E");
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
        v.setTextViewText(R.id.wLight, "свет " + light + "%");

        String foot;
        if (ok > 0) {
            String when = new SimpleDateFormat("HH:mm", Locale.getDefault()).format(new Date(ok));
            foot = (rssi != 0 ? rssi + " дБм · " : "") + when + (time.length() > 0 ? " · " + time : "");
        } else {
            foot = host.length() > 0 ? host : "нажмите, чтобы настроить";
        }
        v.setTextViewText(R.id.wFoot, foot);

        // нажатие на виджет — открыть страницу платы
        if (host.length() > 0) {
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

        // кнопка обновления
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
