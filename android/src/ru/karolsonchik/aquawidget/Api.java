package ru.karolsonchik.aquawidget;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;

/** Загрузка состояния с платы и кэш для виджета. */
public class Api {

    public static final String PREFS = "aqua";
    public static final String KEY_HOST = "host";
    public static final String KEY_NAME = "name";
    public static final String KEY_TEMP = "temp";
    public static final String KEY_SP = "sp";
    public static final String KEY_HEAT = "heat";
    public static final String KEY_LIGHT = "light";
    public static final String KEY_ALARM = "alarm";
    public static final String KEY_RSSI = "rssi";
    public static final String KEY_TIME = "time";
    public static final String KEY_OK = "lastok";
    public static final String KEY_FW = "fw";

    public static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static String host(Context c) {
        return prefs(c).getString(KEY_HOST, "");
    }

    static String get(String url) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(6000);
        c.setReadTimeout(6000);
        c.setRequestProperty("Accept", "application/json");
        c.setRequestProperty("User-Agent", "AquaWidget/1.0");
        try {
            int code = c.getResponseCode();
            if (code != 200) throw new Exception("HTTP " + code);
            InputStream in = c.getInputStream();
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[2048];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            in.close();
            return new String(bo.toByteArray(), "UTF-8");
        } finally {
            c.disconnect();
        }
    }

    /** Читает /api/state и обновляет кэш. Возвращает true при успехе. */
    public static boolean refresh(Context ctx) {
        String h = host(ctx);
        if (h == null || h.length() == 0) return false;
        try {
            String body = get("http://" + h + "/api/state");
            JSONObject j = new JSONObject(body);
            SharedPreferences.Editor e = prefs(ctx).edit();
            e.putString(KEY_NAME, j.optString("name", "Аквариум"));
            e.putString(KEY_FW, j.optString("fw", ""));
            boolean tok = j.optInt("tok", 0) == 1;
            double t = j.optDouble("t", 0);
            e.putString(KEY_TEMP, tok && t != 0 ? String.format(Locale.US, "%.2f", t) : "");
            e.putString(KEY_SP, String.format(Locale.US, "%.1f", j.optDouble("sp", 0)));
            e.putInt(KEY_HEAT, j.optInt("heat", 0));
            int light = 0;
            JSONArray lp = j.optJSONArray("lp");
            if (lp != null && lp.length() > 0) light = lp.optInt(0, 0);
            e.putInt(KEY_LIGHT, light);
            e.putInt(KEY_ALARM, j.optInt("al", 0));
            e.putInt(KEY_RSSI, j.optInt("rssi", 0));
            e.putString(KEY_TIME, j.optString("time", ""));
            e.putLong(KEY_OK, System.currentTimeMillis());
            e.apply();
            return true;
        } catch (Exception ex) {
            return false;
        }
    }
}
