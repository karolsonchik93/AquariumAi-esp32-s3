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
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
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

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(28), dp(20), dp(20));
        root.setBackgroundColor(Color.parseColor("#060B14"));

        TextView title = new TextView(this);
        title.setText("Аквариум — виджет");
        title.setTextColor(Color.parseColor("#E8EEF6"));
        title.setTextSize(22);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("Виджет 2×3 показывает температуру воды, уставку, нагрев и свет с вашей платы. "
                + "Адрес платы подставляется сам, когда вы скачиваете виджет со страницы платы.");
        hint.setTextColor(Color.parseColor("#8B9BAD"));
        hint.setTextSize(13);
        hint.setPadding(0, dp(10), 0, dp(18));
        root.addView(hint);

        TextView l1 = new TextView(this);
        l1.setText("АДРЕС ПЛАТЫ");
        l1.setTextColor(Color.parseColor("#4D5B6B"));
        l1.setTextSize(11);
        root.addView(l1);

        hostInput = new EditText(this);
        hostInput.setSingleLine(true);
        hostInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        hostInput.setHint("192.168.3.204");
        hostInput.setTextColor(Color.parseColor("#E8EEF6"));
        hostInput.setHintTextColor(Color.parseColor("#4D5B6B"));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.parseColor("#132033"));
        bg.setCornerRadius(dp(12));
        hostInput.setBackground(bg);
        hostInput.setPadding(dp(12), dp(10), dp(12), dp(10));
        String h = Api.host(this);
        if (h != null) hostInput.setText(h);
        root.addView(hostInput);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(14), 0, dp(6));

        Button save = new Button(this);
        save.setText("Сохранить");
        save.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String s = hostInput.getText().toString().trim();
                s = s.replace("http://", "").replace("https://", "");
                while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
                if (s.indexOf(':') < 0) s = s;
                if (s.length() == 0) {
                    Toast.makeText(MainActivity.this, "Укажите адрес платы", Toast.LENGTH_SHORT).show();
                    return;
                }
                Api.prefs(MainActivity.this).edit().putString(Api.KEY_HOST, s).apply();
                hostInput.setText(s);
                updateStatus();
                refreshNow();
            }
        });
        row.addView(save);

        Button add = new Button(this);
        add.setText("Добавить виджет");
        add.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                requestPin();
            }
        });
        row.addView(add);
        root.addView(row);

        Button open = new Button(this);
        open.setText("Открыть страницу платы");
        open.setOnClickListener(new View.OnClickListener() {
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
        });
        root.addView(open);

        statusView = new TextView(this);
        statusView.setTextColor(Color.parseColor("#8B9BAD"));
        statusView.setTextSize(13);
        statusView.setPadding(0, dp(18), 0, 0);
        statusView.setGravity(Gravity.START);
        root.addView(statusView);

        return root;
    }

    private void updateStatus() {
        String h = Api.host(this);
        if (h == null || h.length() == 0) {
            statusView.setText("Плата не привязана.\nСкачайте виджет со страницы платы или введите адрес вручную.");
        } else {
            statusView.setText("Плата: " + h + "\nДанные обновляются при открытии виджета и по нажатию «обновить».");
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
                        if (statusView != null) {
                            String h = Api.host(self);
                            statusView.setText((ok ? "Связь есть · " : "Нет ответа от платы · ") + h
                                    + "\nДанные виджета обновлены.");
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
