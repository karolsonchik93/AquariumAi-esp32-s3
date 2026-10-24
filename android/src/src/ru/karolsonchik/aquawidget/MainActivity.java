package ru.karolsonchik.aquawidget;

import android.app.Activity;
import android.appwidget.AppWidgetManager;
import android.content.ComponentName;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/** Экран привязки: получает адрес платы из ссылки aqua://bind?host=… и добавляет виджет. */
public class MainActivity extends Activity {

    private EditText hostInput;
    private TextView statusView;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent intent = getIntent();
        if (intent != null && intent.getData() != null) {
            Uri u = intent.getData();
            String h = u.getQueryParameter("host");
            if (h != null && h.length() > 0) {
                Api.prefs(this).edit().putString(Api.KEY_HOST, h).apply();
            }
        }
        setContentView(buildUi());
        updateStatus();
    }

    private int dp(int v) {
        return Math.round(getResources().getDisplayMetrics().density * v);
    }

    private Button makeButton(String text, View.OnClickListener l) {
        Button btn = new Button(this);
        btn.setText(text);
        btn.setAllCaps(false);
        btn.setTextSize(15);
        btn.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        btn.setLayoutParams(lp);
        return btn;
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.parseColor("#060B14"));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(28), dp(20), dp(28));
        scroll.addView(root);

        TextView title = new TextView(this);
        title.setText("Аквариум — виджет");
        title.setTextColor(Color.parseColor("#E8EEF6"));
        title.setTextSize(23);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("Виджет 2×3 показывает температуру воды, уставку, нагрев и свет с вашей платы.\n"
                + "Адрес платы подставляется автоматически, когда вы скачиваете виджет со страницы платы.");
        hint.setTextColor(Color.parseColor("#8B9BAD"));
        hint.setTextSize(13.5f);
        hint.setPadding(0, dp(12), 0, dp(20));
        root.addView(hint);

        TextView l1 = new TextView(this);
        l1.setText("АДРЕС ПЛАТЫ");
        l1.setTextColor(Color.parseColor("#4D5B6B"));
        l1.setTextSize(11);
        l1.setPadding(0, 0, 0, dp(6));
        root.addView(l1);

        hostInput = new EditText(this);
        hostInput.setSingleLine(true);
        hostInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        hostInput.setHint("192.168.3.204");
        hostInput.setTextColor(Color.parseColor("#E8EEF6"));
        hostInput.setHintTextColor(Color.parseColor("#4D5B6B"));
        hostInput.setTextSize(16);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.parseColor("#132033"));
        bg.setCornerRadius(dp(12));
        hostInput.setBackground(bg);
        hostInput.setPadding(dp(14), dp(12), dp(14), dp(12));
        String h = Api.host(this);
        if (h != null) hostInput.setText(h);
        root.addView(hostInput);

        root.addView(makeButton("Сохранить адрес и проверить связь", new View.OnClickListener() {
            public void onClick(View v) {
                String s = hostInput.getText().toString().trim();
                s = s.replace("http://", "").replace("https://", "");
                while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
                if (s.length() == 0) {
                    Toast.makeText(MainActivity.this, "Укажите адрес платы", Toast.LENGTH_SHORT).show();
                    return;
                }
                Api.prefs(MainActivity.this).edit().putString(Api.KEY_HOST, s).apply();
                hostInput.setText(s);
                updateStatus();
            }
        }));

        root.addView(makeButton("Добавить виджет на главный экран", new View.OnClickListener() {
            public void onClick(View v) {
                requestPin();
            }
        }));

        root.addView(makeButton("Открыть страницу платы", new View.OnClickListener() {
            public void onClick(View v) {
                String s = Api.host(MainActivity.this);
                if (s.length() == 0) {
                    Toast.makeText(MainActivity.this, "Сначала укажите адрес", Toast.LENGTH_SHORT).show();
                    return;
                }
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("http://" + s + "/")));
                } catch (Exception e) {
                    Toast.makeText(MainActivity.this, "Нет браузера", Toast.LENGTH_SHORT).show();
                }
            }
        }));

        statusView = new TextView(this);
        statusView.setTextColor(Color.parseColor("#8B9BAD"));
        statusView.setTextSize(13);
        statusView.setPadding(0, dp(22), 0, 0);
        statusView.setLineSpacing(0, 1.35f);
        root.addView(statusView);

        TextView foot = new TextView(this);
        foot.setText("Если виджет не появился: зажмите пустое место на рабочем столе → «Виджеты» → «Аквариум».");
        foot.setTextColor(Color.parseColor("#3F4B59"));
        foot.setTextSize(11.5f);
        foot.setPadding(0, dp(16), 0, 0);
        root.addView(foot);

        return scroll;
    }

    private void updateStatus() {
        String h = Api.host(this);
        if (h == null || h.length() == 0) {
            statusView.setText("Плата не привязана.\nСкачайте виджет со страницы платы или введите адрес вручную.");
        } else {
            statusView.setText("Плата: " + h + "\nПроверяю связь…");
            refreshNow();
        }
    }

    private void refreshNow() {
        final MainActivity self = this;
        new Thread(new Runnable() {
            public void run() {
                final boolean ok = Api.refresh(self);
                runOnUiThread(new Runnable() {
                    public void run() {
                        if (statusView == null) return;
                        String h = Api.host(self);
                        if (ok) {
                            String t = Api.prefs(self).getString(Api.KEY_TEMP, "");
                            statusView.setText("Связь с платой есть · " + h + "\n"
                                    + (t.length() > 0 ? "Температура воды: " + t + " °C" : "Датчик воды не найден")
                                    + "\nДанные виджета обновлены.");
                        } else {
                            statusView.setText("Плата не ответила · " + h
                                    + "\nПроверьте, что телефон в той же сети, и адрес верный.");
                        }
                        AquaWidgetProvider.push(self);
                    }
                });
            }
        }).start();
    }

    private void requestPin() {
        if (Build.VERSION.SDK_INT >= 26 && getPackageManager().hasSystemFeature(
                android.content.pm.PackageManager.FEATURE_APP_WIDGETS)) {
            AppWidgetManager m = AppWidgetManager.getInstance(this);
            if (m.isRequestPinAppWidgetSupported()) {
                ComponentName cn = new ComponentName(this, AquaWidgetProvider.class);
                m.requestPinAppWidget(cn, null, null);
                Toast.makeText(this, "Подтвердите добавление виджета", Toast.LENGTH_SHORT).show();
                return;
            }
        }
        Toast.makeText(this,
                "Зажмите пустое место на рабочем столе → «Виджеты» → «Аквариум»",
                Toast.LENGTH_LONG).show();
    }
}
