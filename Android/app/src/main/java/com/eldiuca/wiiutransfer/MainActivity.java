package com.eldiuca.wiiutransfer;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Actividad principal de Wii U Transfer Tools para Android v2.0.0 (por El Diuca).
 * Compatible desde Android 9 (API 28) hacia arriba:
 *   - Lenguaje 100% simple ("Modo USB (Cable)" y "Modo Red (Wi-Fi)")
 *   - Botón "i Info" (Acerca de + Enlace a GitHub + Descarga de actualizaciones de APK y App Wii U .wuhb)
 *   - Aviso automático de nueva versión consultando GitHub version.json
 */
public class MainActivity extends Activity {

    public static final String APP_VERSION = "2.0.3";
    public static final String GITHUB_REPO_URL = "https://github.com/supermafari007/WiiU-Transfer-Tools";
    public static final String VERSION_JSON_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/version.json";
    public static final String DEFAULT_APK_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/Android/WiiU_Transfer_Tools_Android.apk";
    public static final String DEFAULT_WUHB_URL = "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/Wii%20U/WiiU_Transfer.wuhb";

    // Paleta Oscura Oficial Wii U Transfer Tools
    private static final int COL_BG        = Color.parseColor("#0F141B");
    private static final int COL_HEADER    = Color.parseColor("#0B3954");
    private static final int COL_CARD      = Color.parseColor("#18202B");
    private static final int COL_CARD_ALT  = Color.parseColor("#1F2937");
    private static final int COL_BOX_SOFT  = Color.parseColor("#121922");
    private static final int COL_BTN_OFF   = Color.parseColor("#253141");
    private static final int COL_BORDER    = Color.parseColor("#2E3D50");
    private static final int COL_CYAN      = Color.parseColor("#00AEEF");
    private static final int COL_CYAN_DK   = Color.parseColor("#4DD4FF");
    private static final int COL_CYAN_LT   = Color.parseColor("#11384D");
    private static final int COL_CYAN_SUB  = Color.parseColor("#A8D8EC");
    private static final int COL_GREEN     = Color.parseColor("#17A95B");
    private static final int COL_GREEN_HI  = Color.parseColor("#2FE082");
    private static final int COL_RED       = Color.parseColor("#D92B3A");
    private static final int COL_AMBER_BG  = Color.parseColor("#3B280B");
    private static final int COL_AMBER_BD  = Color.parseColor("#F59E0B");
    private static final int COL_AMBER_TX  = Color.parseColor("#FCD34D");
    private static final int COL_TEXT      = Color.parseColor("#ECF3F9");
    private static final int COL_MUTED     = Color.parseColor("#8EA3B8");
    private static final int COL_PILL_OFF  = Color.parseColor("#072638");
    private static final int COL_TRACK     = Color.parseColor("#273445");

    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private File activeFolder;
    private boolean scanWholePhone = true;
    private final List<NfsaTransferServer.WupPackage> packages = new ArrayList<NfsaTransferServer.WupPackage>();
    private final Set<String> markedNames = new HashSet<String>();
    private NfsaTransferServer server;
    private UsbAoaServer aoaServer;
    private String aoaStatusMsg = "";
    private WifiManager.WifiLock wifiLock;
    private PowerManager.WakeLock wakeLock;

    private boolean isUsbMtpTab = true;
    private boolean usbDataConnected = false;
    private boolean usbConfigured = false;
    private boolean usbMtpEnabled = false;
    private long lastStatTimeMs = System.currentTimeMillis();

    // Estado de actualización desde GitHub
    private String latestRemoteVersion = APP_VERSION;
    private String remoteApkUrl = DEFAULT_APK_URL;
    private String remoteWuhbUrl = DEFAULT_WUHB_URL;
    private String remoteNotes = "";
    private boolean updateAvailable = false;

    // Vistas de UI
    private LinearLayout headerPillLayout;
    private View headerPillDot;
    private TextView headerPillText;

    private LinearLayout updateBannerLayout;
    private TextView updateBannerText;

    private TextView tabUsbMtp;
    private TextView tabNetMode;

    private LinearLayout connectionCard;
    private TextView connTagText;
    private TextView connTitleText;
    private TextView connSubText;
    private TextView folderPathText;

    private TextView pkgHeaderCountText;
    private TextView btnAllToggle;
    private LinearLayout pkgListContainer;

    private TextView bottomSummaryTitle;
    private TextView bottomSummarySub;
    private View progressFillView;
    private FrameLayout progressTrackLayout;
    private TextView btnPrimaryAction;
    private TextView btnSecondaryAction;

    private static final String ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE";

    private final BroadcastReceiver usbStateReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (intent == null) return;
            boolean conn = intent.getBooleanExtra("connected", false);
            boolean conf = intent.getBooleanExtra("configured", false);
            boolean mtp  = intent.getBooleanExtra("mtp", false) || intent.getBooleanExtra("ptp", false);
            if (conn != usbDataConnected || conf != usbConfigured || mtp != usbMtpEnabled) {
                usbDataConnected = conn;
                usbConfigured = conf;
                usbMtpEnabled = mtp;
                updateConnectionCardUi();
                if (usbDataConnected && usbConfigured && usbMtpEnabled) {
                    syncQueueToUsbAutomatically();
                }
            }
        }
    };

    private final Runnable uiPollRunnable = new Runnable() {
        @Override
        public void run() {
            pollStatsAndRefresh();
            uiHandler.postDelayed(this, 600);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            Window w = getWindow();
            w.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
            w.setStatusBarColor(COL_HEADER);
            w.setNavigationBarColor(COL_BG);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        activeFolder = new File(Environment.getExternalStorageDirectory(), "WiiU_Transfer");
        server = new NfsaTransferServer(activeFolder, new NfsaTransferServer.EventListener() {
            @Override
            public void onLog(final String level, final String message) {
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (bottomSummarySub != null) {
                            String ts = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
                            bottomSummarySub.setText("[" + ts + "] " + message);
                        }
                    }
                });
            }
        });

        aoaServer = new UsbAoaServer(this, activeFolder, new UsbAoaServer.Listener() {
            @Override
            public void onConnectionChanged(final boolean isConn, final String statusText) {
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        aoaStatusMsg = (statusText != null) ? statusText : "";
                        updateConnectionCardUi();
                        updateBottomSummaryUi();
                    }
                });
            }

            @Override
            public void onTransferActivity(final String currentFile, final long totalBytesSent) {
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (isUsbMtpTab && bottomSummarySub != null) {
                            bottomSummarySub.setText("Enviando por USB: " + currentFile +
                                    "  \u2022  Total: " + NfsaTransferServer.formatBytes(totalBytesSent));
                        }
                    }
                });
            }
        });
        aoaServer.register();

        setContentView(buildRootLayout());
        ensureStoragePermission();
        prepareDefaultDirectory();
        refreshPackages();
        updateConnectionCardUi();
        aoaServer.handleIntent(getIntent());
        checkForUpdatesInBackground(false);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (aoaServer != null) {
            aoaServer.handleIntent(intent);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        Intent stickyUsb = registerReceiver(usbStateReceiver, new IntentFilter(ACTION_USB_STATE));
        if (stickyUsb != null) {
            usbDataConnected = stickyUsb.getBooleanExtra("connected", false);
            usbConfigured = stickyUsb.getBooleanExtra("configured", false);
            usbMtpEnabled = stickyUsb.getBooleanExtra("mtp", false) || stickyUsb.getBooleanExtra("ptp", false);
            updateConnectionCardUi();
        }
        if (aoaServer != null) {
            aoaServer.probeExistingAccessories();
        }
        // Siempre re-escanear al abrir o volver a la app para mostrar solo los juegos reales actuales
        refreshPackages();
        uiHandler.post(uiPollRunnable);
    }

    @Override
    protected void onPause() {
        super.onPause();
        try {
            unregisterReceiver(usbStateReceiver);
        } catch (Exception ignored) {
        }
        uiHandler.removeCallbacks(uiPollRunnable);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        releaseNetworkLocks();
        if (aoaServer != null) {
            aoaServer.unregisterAndClose();
        }
        if (server != null) {
            server.stop();
        }
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private static GradientDrawable roundedShape(int fillColor, int radiusPx, int strokeWidthPx, int strokeColor) {
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(fillColor);
        gd.setCornerRadius(radiusPx);
        if (strokeWidthPx > 0) {
            gd.setStroke(strokeWidthPx, strokeColor);
        }
        return gd;
    }

    private static GradientDrawable circleShape(int fillColor) {
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.OVAL);
        gd.setColor(fillColor);
        return gd;
    }

    private View buildRootLayout() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(COL_BG);

        // 1. HEADER OSCURO CON ACENTO CYAN + BOTÓN "i Info"
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setBackgroundColor(COL_HEADER);
        header.setPadding(dp(12), dp(10), dp(12), dp(10));

        ImageView iconView = new ImageView(this);
        iconView.setImageResource(R.drawable.header_icon);
        LinearLayout.LayoutParams iconLp = new LinearLayout.LayoutParams(dp(44), dp(34));
        iconLp.rightMargin = dp(8);
        header.addView(iconView, iconLp);

        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams titleColLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);

        TextView titleTv = new TextView(this);
        titleTv.setText("Wii U Transfer Tools");
        titleTv.setTextColor(Color.WHITE);
        titleTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15.5f);
        titleTv.setTypeface(Typeface.DEFAULT_BOLD);
        titleTv.setSingleLine(true);
        titleCol.addView(titleTv);

        TextView subTv = new TextView(this);
        subTv.setText("por El Diuca  \u2022  v" + APP_VERSION);
        subTv.setTextColor(COL_CYAN_SUB);
        subTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        subTv.setSingleLine(true);
        titleCol.addView(subTv);
        header.addView(titleCol, titleColLp);

        // Botón "i Info" (Acerca de / Actualizaciones / GitHub)
        TextView btnInfo = new TextView(this);
        btnInfo.setText("i Info");
        btnInfo.setTextColor(COL_CYAN_SUB);
        btnInfo.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        btnInfo.setTypeface(Typeface.DEFAULT_BOLD);
        btnInfo.setGravity(Gravity.CENTER);
        btnInfo.setPadding(dp(10), dp(6), dp(10), dp(6));
        btnInfo.setBackground(roundedShape(COL_PILL_OFF, dp(14), dp(1), COL_CYAN));
        btnInfo.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAboutDialog();
            }
        });
        LinearLayout.LayoutParams infoLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        infoLp.rightMargin = dp(6);
        header.addView(btnInfo, infoLp);

        // Cápsula estado derecha
        headerPillLayout = new LinearLayout(this);
        headerPillLayout.setOrientation(LinearLayout.HORIZONTAL);
        headerPillLayout.setGravity(Gravity.CENTER_VERTICAL);
        headerPillLayout.setPadding(dp(9), dp(6), dp(10), dp(6));
        headerPillLayout.setBackground(roundedShape(COL_GREEN, dp(14), 0, 0));

        headerPillDot = new View(this);
        headerPillDot.setBackground(circleShape(Color.parseColor("#B4FFD2")));
        LinearLayout.LayoutParams dotLp = new LinearLayout.LayoutParams(dp(7), dp(7));
        dotLp.rightMargin = dp(5);
        headerPillLayout.addView(headerPillDot, dotLp);

        headerPillText = new TextView(this);
        headerPillText.setText("Cable Listo");
        headerPillText.setTextColor(Color.WHITE);
        headerPillText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        headerPillText.setTypeface(Typeface.DEFAULT_BOLD);
        headerPillLayout.addView(headerPillText);

        header.addView(headerPillLayout);
        root.addView(header, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // Línea de acento Cyan de 2dp
        View cyanLine = new View(this);
        cyanLine.setBackgroundColor(COL_CYAN);
        root.addView(cyanLine, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(2)));

        // CONTENIDO SCROLLABLE + TARJETA INFERIOR FIJA
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout.LayoutParams scrollLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(14), dp(10), dp(14), dp(12));

        // BANNER DE ALERTA DE ACTUALIZACIÓN AUTOMÁTICA (visible si hay nueva versión en GitHub)
        updateBannerLayout = new LinearLayout(this);
        updateBannerLayout.setOrientation(LinearLayout.HORIZONTAL);
        updateBannerLayout.setGravity(Gravity.CENTER_VERTICAL);
        updateBannerLayout.setPadding(dp(12), dp(8), dp(10), dp(8));
        updateBannerLayout.setBackground(roundedShape(COL_AMBER_BG, dp(12), dp(1), COL_AMBER_BD));
        updateBannerLayout.setVisibility(View.GONE);

        updateBannerText = new TextView(this);
        updateBannerText.setText("\u00a1Nueva versi\u00f3n disponible en GitHub!");
        updateBannerText.setTextColor(COL_AMBER_TX);
        updateBannerText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        updateBannerText.setTypeface(Typeface.DEFAULT_BOLD);
        updateBannerLayout.addView(updateBannerText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView btnBannerUpdate = new TextView(this);
        btnBannerUpdate.setText("Actualizar");
        btnBannerUpdate.setTextColor(Color.parseColor("#111827"));
        btnBannerUpdate.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        btnBannerUpdate.setTypeface(Typeface.DEFAULT_BOLD);
        btnBannerUpdate.setPadding(dp(10), dp(5), dp(10), dp(5));
        btnBannerUpdate.setBackground(roundedShape(COL_AMBER_BD, dp(8), 0, 0));
        btnBannerUpdate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAboutDialog();
            }
        });
        updateBannerLayout.addView(btnBannerUpdate);

        LinearLayout.LayoutParams ubLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        ubLp.bottomMargin = dp(10);
        content.addView(updateBannerLayout, ubLp);

        // 2. SELECTOR SIMPLE DE 2 MODOS: [ Modo USB (Cable) ] | [ Modo Red (Wi-Fi) ]
        LinearLayout segBar = new LinearLayout(this);
        segBar.setOrientation(LinearLayout.HORIZONTAL);
        segBar.setPadding(dp(4), dp(4), dp(4), dp(4));
        segBar.setBackground(roundedShape(COL_PILL_OFF, dp(22), dp(1), COL_BORDER));

        tabUsbMtp = new TextView(this);
        tabUsbMtp.setText("Modo USB (Cable)");
        tabUsbMtp.setGravity(Gravity.CENTER);
        tabUsbMtp.setPadding(dp(8), dp(9), dp(8), dp(9));
        tabUsbMtp.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        tabUsbMtp.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                switchModeTab(true);
            }
        });
        segBar.addView(tabUsbMtp, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        tabNetMode = new TextView(this);
        tabNetMode.setText("Modo Red (Wi-Fi)");
        tabNetMode.setGravity(Gravity.CENTER);
        tabNetMode.setPadding(dp(8), dp(9), dp(8), dp(9));
        tabNetMode.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        tabNetMode.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                switchModeTab(false);
            }
        });
        segBar.addView(tabNetMode, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        LinearLayout.LayoutParams segLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        segLp.bottomMargin = dp(10);
        content.addView(segBar, segLp);

        // 3. TARJETA DE CONEXIÓN EN LENGUAJE SIMPLE
        connectionCard = new LinearLayout(this);
        connectionCard.setOrientation(LinearLayout.VERTICAL);
        connectionCard.setPadding(dp(14), dp(12), dp(14), dp(12));
        connectionCard.setBackground(roundedShape(COL_CARD, dp(16), dp(2), COL_CYAN));

        connTagText = new TextView(this);
        connTagText.setText("CONECTAR POR CABLE USB A LA WII U");
        connTagText.setTextColor(COL_CYAN_DK);
        connTagText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        connTagText.setTypeface(Typeface.DEFAULT_BOLD);
        connectionCard.addView(connTagText);

        connTitleText = new TextView(this);
        connTitleText.setText("Conecta Android al USB frontal de la Wii U");
        connTitleText.setTextColor(Color.WHITE);
        connTitleText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        connTitleText.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams ctLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        ctLp.topMargin = dp(3);
        connectionCard.addView(connTitleText, ctLp);

        connSubText = new TextView(this);
        connSubText.setText("En Android elige 'Transferencia de archivos' y en Wii U 'Modo USB'");
        connSubText.setTextColor(COL_MUTED);
        connSubText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        LinearLayout.LayoutParams csLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        csLp.topMargin = dp(2);
        csLp.bottomMargin = dp(10);
        connectionCard.addView(connSubText, csLp);

        // Caja de carpeta de juegos con botón Cambiar
        LinearLayout folderBox = new LinearLayout(this);
        folderBox.setOrientation(LinearLayout.HORIZONTAL);
        folderBox.setGravity(Gravity.CENTER_VERTICAL);
        folderBox.setPadding(dp(10), dp(6), dp(6), dp(6));
        folderBox.setBackground(roundedShape(COL_BOX_SOFT, dp(8), dp(1), COL_BORDER));

        folderPathText = new TextView(this);
        folderPathText.setText("Carpeta de juegos:  / almacenamiento / WiiU_Transfer");
        folderPathText.setTextColor(COL_CYAN_SUB);
        folderPathText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        folderPathText.setSingleLine(true);
        folderPathText.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        folderBox.addView(folderPathText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView btnChangeFolder = new TextView(this);
        btnChangeFolder.setText("Cambiar");
        btnChangeFolder.setTextColor(COL_TEXT);
        btnChangeFolder.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        btnChangeFolder.setTypeface(Typeface.DEFAULT_BOLD);
        btnChangeFolder.setPadding(dp(10), dp(5), dp(10), dp(5));
        btnChangeFolder.setBackground(roundedShape(COL_BTN_OFF, dp(6), dp(1), COL_BORDER));
        btnChangeFolder.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showChangeFolderDialog();
            }
        });
        folderBox.addView(btnChangeFolder);
        connectionCard.addView(folderBox);

        LinearLayout.LayoutParams ccLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        ccLp.bottomMargin = dp(10);
        content.addView(connectionCard, ccLp);

        // 4. TARJETA LISTA DE JUEGOS EN EL CELULAR
        LinearLayout pkgCard = new LinearLayout(this);
        pkgCard.setOrientation(LinearLayout.VERTICAL);
        pkgCard.setPadding(dp(14), dp(12), dp(14), dp(12));
        pkgCard.setBackground(roundedShape(COL_CARD, dp(16), dp(1), COL_BORDER));

        pkgHeaderCountText = new TextView(this);
        pkgHeaderCountText.setText("Juegos en Android (0)");
        pkgHeaderCountText.setTextColor(COL_TEXT);
        pkgHeaderCountText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        pkgHeaderCountText.setTypeface(Typeface.DEFAULT_BOLD);
        pkgCard.addView(pkgHeaderCountText);

        LinearLayout pkgBtnRow = new LinearLayout(this);
        pkgBtnRow.setOrientation(LinearLayout.HORIZONTAL);
        pkgBtnRow.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams brLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        brLp.topMargin = dp(8);

        TextView btnAddGame = new TextView(this);
        btnAddGame.setText("+ A\u00f1adir Juego");
        btnAddGame.setTextColor(COL_TEXT);
        btnAddGame.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        btnAddGame.setTypeface(Typeface.DEFAULT_BOLD);
        btnAddGame.setGravity(Gravity.CENTER);
        btnAddGame.setPadding(dp(10), dp(7), dp(10), dp(7));
        btnAddGame.setBackground(roundedShape(COL_BTN_OFF, dp(8), dp(1), COL_BORDER));
        btnAddGame.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAddGameDialog();
            }
        });
        LinearLayout.LayoutParams addLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.15f);
        addLp.rightMargin = dp(6);
        pkgBtnRow.addView(btnAddGame, addLp);

        TextView btnRefreshList = new TextView(this);
        btnRefreshList.setText("Actualizar lista");
        btnRefreshList.setTextColor(Color.WHITE);
        btnRefreshList.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        btnRefreshList.setTypeface(Typeface.DEFAULT_BOLD);
        btnRefreshList.setGravity(Gravity.CENTER);
        btnRefreshList.setPadding(dp(10), dp(7), dp(10), dp(7));
        btnRefreshList.setBackground(roundedShape(COL_CYAN_LT, dp(8), dp(1), COL_CYAN));
        btnRefreshList.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                refreshPackages();
                Toast.makeText(MainActivity.this,
                        "Lista actualizada (" + packages.size() + " juego" + (packages.size() == 1 ? "" : "s") + ")",
                        Toast.LENGTH_SHORT).show();
            }
        });
        LinearLayout.LayoutParams refLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.15f);
        refLp.rightMargin = dp(6);
        pkgBtnRow.addView(btnRefreshList, refLp);

        btnAllToggle = new TextView(this);
        btnAllToggle.setText("Todos");
        btnAllToggle.setTextColor(COL_CYAN_DK);
        btnAllToggle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        btnAllToggle.setTypeface(Typeface.DEFAULT_BOLD);
        btnAllToggle.setGravity(Gravity.CENTER);
        btnAllToggle.setPadding(dp(10), dp(7), dp(10), dp(7));
        btnAllToggle.setBackground(roundedShape(COL_CYAN_LT, dp(8), dp(1), COL_CYAN));
        btnAllToggle.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleSelectAll();
            }
        });
        pkgBtnRow.addView(btnAllToggle, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.75f));
        pkgCard.addView(pkgBtnRow, brLp);

        pkgListContainer = new LinearLayout(this);
        pkgListContainer.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams listLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        listLp.topMargin = dp(10);
        pkgCard.addView(pkgListContainer, listLp);

        content.addView(pkgCard, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        scroll.addView(content);
        root.addView(scroll, scrollLp);

        // 5. TARJETA INFERIOR CONTEXTUAL (Modo USB vs Modo Red)
        LinearLayout bottomCard = new LinearLayout(this);
        bottomCard.setOrientation(LinearLayout.VERTICAL);
        bottomCard.setPadding(dp(14), dp(12), dp(14), dp(14));
        bottomCard.setBackground(roundedShape(COL_CARD, dp(16), dp(1), COL_BORDER));
        LinearLayout.LayoutParams botLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        botLp.setMargins(dp(14), 0, dp(14), dp(12));

        bottomSummaryTitle = new TextView(this);
        bottomSummaryTitle.setText("0 juegos seleccionados (0 B)  \u2022  Listo para Modo USB");
        bottomSummaryTitle.setTextColor(COL_CYAN_DK);
        bottomSummaryTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        bottomSummaryTitle.setTypeface(Typeface.DEFAULT_BOLD);
        bottomCard.addView(bottomSummaryTitle);

        bottomSummarySub = new TextView(this);
        bottomSummarySub.setText("En tu Wii U elige 'Modo USB' [X] y pulsa [Y] para ver tus juegos");
        bottomSummarySub.setTextColor(COL_TEXT);
        bottomSummarySub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
        bottomSummarySub.setSingleLine(true);
        bottomSummarySub.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams bsLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        bsLp.topMargin = dp(3);
        bsLp.bottomMargin = dp(8);
        bottomCard.addView(bottomSummarySub, bsLp);

        // Barra de progreso
        progressTrackLayout = new FrameLayout(this);
        progressTrackLayout.setBackground(roundedShape(COL_TRACK, dp(6), 0, 0));
        progressFillView = new View(this);
        progressFillView.setBackground(roundedShape(COL_CYAN, dp(6), 0, 0));
        progressTrackLayout.addView(progressFillView, new FrameLayout.LayoutParams(dp(4), dp(12)));
        LinearLayout.LayoutParams progLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(12));
        progLp.bottomMargin = dp(10);
        bottomCard.addView(progressTrackLayout, progLp);

        // Fila de botones inferiores contextuales
        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);

        btnPrimaryAction = new TextView(this);
        btnPrimaryAction.setText("Preparar Juegos para USB (0)");
        btnPrimaryAction.setTextColor(Color.WHITE);
        btnPrimaryAction.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
        btnPrimaryAction.setTypeface(Typeface.DEFAULT_BOLD);
        btnPrimaryAction.setGravity(Gravity.CENTER);
        btnPrimaryAction.setPadding(dp(12), dp(11), dp(12), dp(11));
        btnPrimaryAction.setBackground(roundedShape(COL_CYAN, dp(10), 0, 0));
        btnPrimaryAction.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onPrimaryActionClicked();
            }
        });
        LinearLayout.LayoutParams pBtnLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        btnRow.addView(btnPrimaryAction, pBtnLp);

        btnSecondaryAction = new TextView(this);
        btnSecondaryAction.setText("Ajustes USB");
        btnSecondaryAction.setTextColor(COL_CYAN_SUB);
        btnSecondaryAction.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        btnSecondaryAction.setTypeface(Typeface.DEFAULT_BOLD);
        btnSecondaryAction.setGravity(Gravity.CENTER);
        btnSecondaryAction.setPadding(dp(14), dp(11), dp(14), dp(11));
        btnSecondaryAction.setBackground(roundedShape(COL_BTN_OFF, dp(10), dp(1), COL_CYAN));
        btnSecondaryAction.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openUsbSettings();
            }
        });
        LinearLayout.LayoutParams sBtnLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        sBtnLp.leftMargin = dp(8);
        btnRow.addView(btnSecondaryAction, sBtnLp);

        bottomCard.addView(btnRow);
        root.addView(bottomCard, botLp);

        updateTabsAppearance();
        return root;
    }

    private void switchModeTab(boolean usbMtp) {
        isUsbMtpTab = usbMtp;
        updateTabsAppearance();
        updateConnectionCardUi();
        updateBottomSummaryUi();
    }

    private void updateTabsAppearance() {
        if (isUsbMtpTab) {
            tabUsbMtp.setBackground(roundedShape(COL_CYAN, dp(18), 0, 0));
            tabUsbMtp.setTextColor(Color.WHITE);
            tabUsbMtp.setTypeface(Typeface.DEFAULT_BOLD);

            tabNetMode.setBackgroundColor(Color.TRANSPARENT);
            tabNetMode.setTextColor(COL_CYAN_SUB);
            tabNetMode.setTypeface(Typeface.DEFAULT);
        } else {
            tabNetMode.setBackground(roundedShape(COL_CYAN, dp(18), 0, 0));
            tabNetMode.setTextColor(Color.WHITE);
            tabNetMode.setTypeface(Typeface.DEFAULT_BOLD);

            tabUsbMtp.setBackgroundColor(Color.TRANSPARENT);
            tabUsbMtp.setTextColor(COL_CYAN_SUB);
            tabUsbMtp.setTypeface(Typeface.DEFAULT);
        }
    }

    private void ensureStoragePermission() {
        if (Build.VERSION.SDK_INT >= 30) {
            if (!Environment.isExternalStorageManager()) {
                try {
                    Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                    i.setData(Uri.parse("package:" + getPackageName()));
                    startActivity(i);
                } catch (Exception e) {
                    try {
                        Intent i = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
                        startActivity(i);
                    } catch (Exception ignored) {
                    }
                }
            }
        } else if (Build.VERSION.SDK_INT >= 23) {
            if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED ||
                checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{
                        Manifest.permission.READ_EXTERNAL_STORAGE,
                        Manifest.permission.WRITE_EXTERNAL_STORAGE
                }, 100);
            }
        }
    }

    private void prepareDefaultDirectory() {
        try {
            if (!activeFolder.exists()) {
                activeFolder.mkdirs();
            }
            File readme = new File(activeFolder, "COMO_USAR.txt");
            if (!readme.exists()) {
                FileOutputStream fos = new FileOutputStream(readme);
                String info = "Coloca aqui tus carpetas de juegos de Wii U (con title.tmd, title.tik y archivos .app).\n" +
                              "- Modo USB (Cable): Conecta tu Android al puerto USB frontal de la Wii U en modo 'Transferencia de archivos' y elige 'Modo USB' en la consola.\n" +
                              "- Modo Red (Wi-Fi): Pulsa 'Compartir Juegos por Red' y elige 'Modo Red' en la consola.\n";
                fos.write(info.getBytes("UTF-8"));
                fos.close();
                MediaScannerConnection.scanFile(this, new String[]{readme.getAbsolutePath()}, null, null);
            }
        } catch (Exception ignored) {
        }
    }

    private List<File> getAllStorageRoots() {
        List<File> roots = new ArrayList<File>();
        File primary = Environment.getExternalStorageDirectory();
        if (primary != null && primary.isDirectory()) {
            roots.add(primary);
        }
        // Detectar tambien tarjetas microSD externas en /storage/XXXX-XXXX
        File storageDir = new File("/storage");
        File[] volumes = storageDir.listFiles();
        if (volumes != null) {
            for (File v : volumes) {
                if (v.isDirectory() && v.canRead()
                        && !v.getName().equalsIgnoreCase("emulated")
                        && !v.getName().equalsIgnoreCase("self")) {
                    roots.add(v);
                }
            }
        }
        return roots;
    }

    private void refreshPackages() {
        List<NfsaTransferServer.WupPackage> scanned;
        if (scanWholePhone) {
            scanned = NfsaTransferServer.scanPackagesMultiRoot(getAllStorageRoots(), markedNames);
            if (folderPathText != null) {
                folderPathText.setText("Todo Android (cualquier carpeta)");
            }
        } else {
            scanned = NfsaTransferServer.scanPackages(activeFolder, markedNames);
            if (folderPathText != null && activeFolder != null) {
                folderPathText.setText(activeFolder.getAbsolutePath());
            }
        }
        packages.clear();
        packages.addAll(scanned);

        // Purgar cualquier juego que ya haya sido borrado de Android
        Set<String> existingNames = new HashSet<String>();
        for (NfsaTransferServer.WupPackage p : packages) {
            existingNames.add(p.name);
        }
        markedNames.retainAll(existingNames);

        if (markedNames.isEmpty()) {
            for (NfsaTransferServer.WupPackage p : packages) {
                if (p.hasTmd) {
                    p.marked = true;
                    markedNames.add(p.name);
                }
            }
        } else {
            for (NfsaTransferServer.WupPackage p : packages) {
                p.marked = markedNames.contains(p.name);
            }
        }

        server.setDiscoveredPackages(packages);
        server.setMarkedQueue(markedNames, true);
        if (aoaServer != null) {
            aoaServer.setRootDir(activeFolder);
            aoaServer.setDiscoveredPackages(packages);
            aoaServer.setMarkedNames(markedNames);
        }
        rebuildPackageListViews();
        updateBottomSummaryUi();
        syncQueueToUsbAutomatically();
    }

    private void rebuildPackageListViews() {
        pkgListContainer.removeAllViews();
        pkgHeaderCountText.setText("Juegos en Android (" + packages.size() + ")");

        int markedCount = 0;
        for (NfsaTransferServer.WupPackage p : packages) {
            if (p.marked) markedCount++;
        }
        boolean allMarked = (!packages.isEmpty() && markedCount == packages.size());
        btnAllToggle.setText(allMarked ? "Limpiar" : "Todos");

        String shortFolder = activeFolder.getAbsolutePath()
                .replace(Environment.getExternalStorageDirectory().getAbsolutePath(), "/ almacenamiento");
        folderPathText.setText("Carpeta de juegos:  " + shortFolder);

        if (packages.isEmpty()) {
            LinearLayout emptyBox = new LinearLayout(this);
            emptyBox.setOrientation(LinearLayout.VERTICAL);
            emptyBox.setPadding(dp(14), dp(18), dp(14), dp(18));
            emptyBox.setBackground(roundedShape(COL_BOX_SOFT, dp(12), dp(1), COL_BORDER));

            TextView emptyTitle = new TextView(this);
            emptyTitle.setText("No hay juegos en " + activeFolder.getName());
            emptyTitle.setTextColor(COL_TEXT);
            emptyTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f);
            emptyTitle.setTypeface(Typeface.DEFAULT_BOLD);
            emptyBox.addView(emptyTitle);

            TextView emptySub = new TextView(this);
            emptySub.setText("Pulsa '+ A\u00f1adir Juego' para importar o descomprimir un .ZIP, o copia tus carpetas de juegos dentro de /WiiU_Transfer o /Download.");
            emptySub.setTextColor(COL_MUTED);
            emptySub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
            LinearLayout.LayoutParams esLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            esLp.topMargin = dp(4);
            emptyBox.addView(emptySub, esLp);

            pkgListContainer.addView(emptyBox);
            return;
        }

        String streamingFile = server.getCurrentStreamingFile();

        for (final NfsaTransferServer.WupPackage pkg : packages) {
            boolean isStreamingThis = (!streamingFile.isEmpty() && streamingFile.startsWith(pkg.name + "/"));
            int cardFill = isStreamingThis ? COL_CYAN_LT : COL_CARD_ALT;
            int cardStroke = isStreamingThis ? COL_CYAN : (pkg.marked ? COL_CYAN_DK : COL_BORDER);
            int strokeW = (isStreamingThis || pkg.marked) ? dp(2) : dp(1);

            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(12), dp(10), dp(12), dp(10));
            row.setBackground(roundedShape(cardFill, dp(12), strokeW, cardStroke));

            // Casilla checkbox
            TextView chk = new TextView(this);
            chk.setGravity(Gravity.CENTER);
            chk.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            chk.setTypeface(Typeface.DEFAULT_BOLD);
            if (pkg.marked) {
                chk.setText("\u2713");
                chk.setTextColor(Color.WHITE);
                chk.setBackground(roundedShape(COL_CYAN, dp(6), 0, 0));
            } else {
                chk.setText("");
                chk.setBackground(roundedShape(COL_BOX_SOFT, dp(6), dp(2), COL_BORDER));
            }
            LinearLayout.LayoutParams chkLp = new LinearLayout.LayoutParams(dp(26), dp(26));
            chkLp.rightMargin = dp(12);
            row.addView(chk, chkLp);

            // Nombre y detalles simples del juego
            LinearLayout infoCol = new LinearLayout(this);
            infoCol.setOrientation(LinearLayout.VERTICAL);
            LinearLayout.LayoutParams infoLp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);

            TextView nameTv = new TextView(this);
            nameTv.setText(pkg.name);
            nameTv.setTextColor(Color.WHITE);
            nameTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
            nameTv.setTypeface(Typeface.DEFAULT_BOLD);
            nameTv.setSingleLine(true);
            nameTv.setEllipsize(TextUtils.TruncateAt.END);
            infoCol.addView(nameTv);

            String stateDesc = pkg.hasTmd ? "Juego completo" : "Carpeta de archivos";
            TextView subInfoTv = new TextView(this);
            subInfoTv.setText(NfsaTransferServer.formatBytes(pkg.totalBytes) + "  \u2022  " + stateDesc);
            subInfoTv.setTextColor(isStreamingThis ? COL_CYAN_DK : COL_MUTED);
            subInfoTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f);
            infoCol.addView(subInfoTv);
            row.addView(infoCol, infoLp);

            // Insignia simple (COPIANDO / SELECCIONADO / DISPONIBLE)
            TextView badge = new TextView(this);
            String badgeTxt = isStreamingThis
                    ? "COPIANDO"
                    : (pkg.marked ? "SELECCIONADO" : "DISPONIBLE");
            int badgeCol = isStreamingThis ? COL_CYAN : (pkg.marked ? COL_GREEN : COL_BTN_OFF);
            badge.setText(badgeTxt);
            badge.setTextColor(Color.WHITE);
            badge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9.5f);
            badge.setTypeface(Typeface.DEFAULT_BOLD);
            badge.setPadding(dp(8), dp(5), dp(8), dp(5));
            badge.setBackground(roundedShape(badgeCol, dp(10), 0, 0));
            row.addView(badge);

            row.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    pkg.marked = !pkg.marked;
                    if (pkg.marked) markedNames.add(pkg.name);
                    else markedNames.remove(pkg.name);
                    server.setMarkedQueue(markedNames, true);
                    rebuildPackageListViews();
                    updateBottomSummaryUi();
                    syncQueueToUsbAutomatically();
                }
            });

            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            rowLp.bottomMargin = dp(8);
            pkgListContainer.addView(row, rowLp);
        }
    }

    private void toggleSelectAll() {
        int markedCount = 0;
        for (NfsaTransferServer.WupPackage p : packages) {
            if (p.marked) markedCount++;
        }
        boolean target = (markedCount < packages.size());
        markedNames.clear();
        for (NfsaTransferServer.WupPackage p : packages) {
            p.marked = target;
            if (target) markedNames.add(p.name);
        }
        server.setMarkedQueue(markedNames, true);
        rebuildPackageListViews();
        updateBottomSummaryUi();
        syncQueueToUsbAutomatically();
    }

    private void updateConnectionCardUi() {
        boolean netRunning = (server != null && server.isRunning());
        String ip = NfsaTransferServer.getLocalIpAddress();

        if (isUsbMtpTab) {
            boolean aoaConn = (aoaServer != null && aoaServer.isConnected());
            connTagText.setText("CONECTAR POR CABLE USB DIRECTO (AOA)");
            if (aoaConn) {
                connTitleText.setText("Conectado por USB directo a la Wii U");
                connSubText.setText("Canal directo activo (sin Depuraci\u00f3n USB). En Wii U pulsa [Y] o [A] para instalar.");
                headerPillLayout.setBackground(roundedShape(COL_GREEN, dp(14), 0, 0));
                headerPillDot.setBackground(circleShape(Color.parseColor("#B4FFD2")));
                headerPillText.setText("Cable Listo");
            } else if (usbDataConnected) {
                connTitleText.setText("Cable USB detectado  \u2022  Pulsa [Y] en Wii U");
                connSubText.setText(aoaStatusMsg.isEmpty()
                        ? "En tu Wii U elige 'Modo USB', pulsa [Y] y toca Aceptar si sale un aviso USB."
                        : aoaStatusMsg);
                headerPillLayout.setBackground(roundedShape(COL_AMBER_BG, dp(14), dp(1), COL_AMBER_BD));
                headerPillDot.setBackground(circleShape(COL_AMBER_TX));
                headerPillText.setText("Esperando Wii U");
            } else {
                connTitleText.setText("Conecta Android al USB frontal de la Wii U");
                connSubText.setText("No necesitas activar Depuraci\u00f3n USB. Solo conecta el cable y pulsa [Y] en Wii U.");
                headerPillLayout.setBackground(roundedShape(COL_PILL_OFF, dp(14), 0, 0));
                headerPillDot.setBackground(circleShape(COL_MUTED));
                headerPillText.setText("Sin Conexi\u00f3n");
            }
        } else {
            connTagText.setText("ENVIAR JUEGOS POR WI-FI A LA WII U");
            if (netRunning) {
                int clients = server.getActiveClientCount();
                connTitleText.setText("Compartiendo por Wi-Fi  \u2022  " + ip);
                connSubText.setText("Consolas Wii U conectadas: " + clients + "  \u2022  Enviado: " +
                        NfsaTransferServer.formatBytes(server.getBytesSentTotal()));
                headerPillLayout.setBackground(roundedShape(COL_GREEN, dp(14), 0, 0));
                headerPillDot.setBackground(circleShape(Color.parseColor("#B4FFD2")));
                headerPillText.setText("En L\u00ednea");
            } else {
                connTitleText.setText("Listo para compartir  \u2022  Wi-Fi: " + ip);
                connSubText.setText("Pulsa 'Compartir Juegos por Red' y en tu Wii U elige 'Modo Red'");
                headerPillLayout.setBackground(roundedShape(COL_PILL_OFF, dp(14), 0, 0));
                headerPillDot.setBackground(circleShape(COL_MUTED));
                headerPillText.setText("En Espera");
            }
        }
    }

    private void updateBottomSummaryUi() {
        int markedCount = 0;
        long markedBytes = 0L;
        for (NfsaTransferServer.WupPackage p : packages) {
            if (p.marked) {
                markedCount++;
                markedBytes += p.totalBytes;
            }
        }

        String modeLabel = isUsbMtpTab ? "Sincronizaci\u00f3n USB autom\u00e1tica" : "Modo Red Wi-Fi";
        bottomSummaryTitle.setText(markedCount + (markedCount == 1 ? " juego seleccionado (" : " juegos seleccionados (") +
                NfsaTransferServer.formatBytes(markedBytes) + ")  \u2022  " + modeLabel);

        if (isUsbMtpTab) {
            boolean aoaConn = (aoaServer != null && aoaServer.isConnected());
            btnPrimaryAction.setText(aoaConn ? "Conexi\u00f3n USB Directa Activa" : "Conectar USB Directo");
            btnPrimaryAction.setBackground(roundedShape(aoaConn ? COL_GREEN : COL_CYAN, dp(10), 0, 0));
            btnSecondaryAction.setVisibility(View.GONE);
            bottomSummarySub.setText(aoaConn
                    ? "Conectado y sincronizado. En Wii U pulsa [Y] para listar y [A] para instalar."
                    : "Selecciona tus juegos arriba, conecta el cable USB y pulsa [Y] en tu Wii U.");
        } else {
            boolean netRunning = (server != null && server.isRunning());
            btnPrimaryAction.setText(netRunning
                    ? "Detener Env\u00edo por Red"
                    : "Compartir Juegos por Red (" + markedCount + ")");
            btnPrimaryAction.setBackground(roundedShape(netRunning ? COL_RED : COL_CYAN, dp(10), 0, 0));
            btnSecondaryAction.setVisibility(View.GONE);
            if (!netRunning) {
                bottomSummarySub.setText("En tu Wii U elige 'Modo Red' [X] y pulsa [Y] para conectar por Wi-Fi");
            }
        }

        int trackW = progressTrackLayout.getWidth();
        if (trackW > 0) {
            float ratio = packages.isEmpty() ? 0f : ((float) markedCount / (float) packages.size());
            if (usbDataConnected && usbMtpEnabled && markedCount > 0) ratio = 1.0f;
            int fillW = Math.max(dp(6), (int) (trackW * ratio));
            progressFillView.setLayoutParams(new FrameLayout.LayoutParams(fillW, dp(12)));
        }
    }

    private void onPrimaryActionClicked() {
        if (!isUsbMtpTab) {
            toggleNetworkServer();
            return;
        }
        if (aoaServer != null) {
            aoaServer.probeExistingAccessories();
            if (aoaServer.isConnected()) {
                Toast.makeText(this, "USB directo ya est\u00e1 conectado a tu Wii U", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "En tu Wii U elige 'Modo USB' y pulsa [Y]", Toast.LENGTH_LONG).show();
            }
        }
    }

    private void syncQueueToUsbAutomatically() {
        if (aoaServer != null) {
            aoaServer.setRootDir(activeFolder);
            aoaServer.setMarkedNames(markedNames);
        }
        final File folderSnap = activeFolder;
        final List<File> selectedDirs = new ArrayList<File>();
        final List<String> selectedNames = new ArrayList<String>();
        for (NfsaTransferServer.WupPackage p : packages) {
            if (p.marked) {
                selectedDirs.add(p.directory);
                selectedNames.add(p.name);
            }
        }

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    File extRoot = Environment.getExternalStorageDirectory();
                    File metaDir = new File(extRoot, "WiiU_Transfer");
                    if (!metaDir.exists()) metaDir.mkdirs();
                    File queueFile = new File(metaDir, "wiiu_queue.txt");

                    String rootPath = extRoot.getCanonicalPath();
                    String activePath = folderSnap.getCanonicalPath();
                    String relFolder = "";
                    if (activePath.startsWith(rootPath)) {
                        relFolder = activePath.substring(rootPath.length()).replace('\\', '/');
                        while (relFolder.startsWith("/")) relFolder = relFolder.substring(1);
                    }

                    StringBuilder sb = new StringBuilder();
                    sb.append("FOLDER=").append(relFolder).append("\n");
                    for (String name : selectedNames) {
                        sb.append("GAME=").append(name).append("\n");
                    }
                    FileOutputStream fos = new FileOutputStream(queueFile);
                    fos.write(sb.toString().getBytes("UTF-8"));
                    fos.close();

                    List<String> pathsToIndex = new ArrayList<String>();
                    pathsToIndex.add(queueFile.getAbsolutePath());
                    for (File d : selectedDirs) {
                        collectAllPaths(d, pathsToIndex);
                    }
                    if (!pathsToIndex.isEmpty()) {
                        MediaScannerConnection.scanFile(
                                getApplicationContext(),
                                pathsToIndex.toArray(new String[0]),
                                null,
                                null);
                    }
                } catch (Exception ignored) {
                }
            }
        }, "Auto-MTP-Sync").start();
    }

    private void openUsbSettings() {
        String[][] candidates = new String[][]{
                {"com.android.settings", "com.android.settings.Settings$UsbDetailsActivity"},
                {"com.android.settings", "com.android.settings.UsbSettings"},
                {"com.android.settings", "com.android.settings.deviceinfo.UsbModeChooserActivity"},
                {"com.android.settings", "com.android.settings.TetherSettings"}
        };
        for (String[] c : candidates) {
            try {
                Intent i = new Intent();
                i.setComponent(new ComponentName(c[0], c[1]));
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(i);
                return;
            } catch (Exception ignored) {
            }
        }
        try {
            startActivity(new Intent(Settings.ACTION_DEVICE_INFO_SETTINGS));
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_SETTINGS));
            } catch (Exception ignored) {
            }
        }
    }

    private static void collectAllPaths(File dir, List<String> outPaths) {
        if (dir == null || !dir.exists()) return;
        outPaths.add(dir.getAbsolutePath());
        File[] children = dir.listFiles();
        if (children == null) return;
        for (File c : children) {
            if (c.isDirectory()) {
                collectAllPaths(c, outPaths);
            } else {
                outPaths.add(c.getAbsolutePath());
            }
        }
    }

    private void acquireNetworkLocks() {
        try {
            if (wifiLock == null) {
                WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wm != null) {
                    int mode = (Build.VERSION.SDK_INT >= 29)
                            ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                            : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
                    wifiLock = wm.createWifiLock(mode, "WiiUTransfer:WifiPerf");
                    wifiLock.setReferenceCounted(false);
                }
            }
            if (wifiLock != null && !wifiLock.isHeld()) {
                wifiLock.acquire();
            }
        } catch (Exception ignored) {
        }
        try {
            if (wakeLock == null) {
                PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
                if (pm != null) {
                    wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "WiiUTransfer:CpuPerf");
                    wakeLock.setReferenceCounted(false);
                }
            }
            if (wakeLock != null && !wakeLock.isHeld()) {
                wakeLock.acquire(4 * 60 * 60 * 1000L);
            }
        } catch (Exception ignored) {
        }
    }

    private void releaseNetworkLocks() {
        try {
            if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        } catch (Exception ignored) {
        }
        try {
            if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        } catch (Exception ignored) {
        }
    }

    private void toggleNetworkServer() {
        if (server.isRunning()) {
            server.stop();
            releaseNetworkLocks();
        } else {
            try {
                acquireNetworkLocks();
                server.setRootDir(activeFolder);
                server.setMarkedQueue(markedNames, true);
                server.start();
            } catch (IOException e) {
                releaseNetworkLocks();
                Toast.makeText(this, "Error al compartir por Red: " + e.getMessage(), Toast.LENGTH_LONG).show();
            }
        }
        updateConnectionCardUi();
        updateBottomSummaryUi();
    }

    private void pollStatsAndRefresh() {
        long now = System.currentTimeMillis();
        long elapsedMs = Math.max(1L, now - lastStatTimeMs);
        lastStatTimeMs = now;

        if (server != null && server.isRunning()) {
            long winBytes = server.getAndResetWindowBytes();
            double mbps = (winBytes / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
            String streamFile = server.getCurrentStreamingFile();
            if (!streamFile.isEmpty()) {
                bottomSummarySub.setText(String.format(Locale.US,
                        "Enviando a Wii U: %s  \u2022  %.2f MB/s", streamFile, mbps));
                rebuildPackageListViews();
            }
            updateConnectionCardUi();
        }
    }

    // =========================================================================
    // Verificador de Actualizaciones en GitHub + Modal "Acerca de"
    // =========================================================================
    private static boolean isRemoteVersionNewer(String currentVer, String remoteVer) {
        if (remoteVer == null || remoteVer.isEmpty()) return false;
        String c = currentVer.replaceAll("[^0-9.]", "");
        String r = remoteVer.replaceAll("[^0-9.]", "");
        String[] cp = c.split("\\.");
        String[] rp = r.split("\\.");
        int len = Math.max(cp.length, rp.length);
        for (int i = 0; i < len; i++) {
            int cv = 0;
            int rv = 0;
            try { if (i < cp.length) cv = Integer.parseInt(cp[i]); } catch (Exception ignored) {}
            try { if (i < rp.length) rv = Integer.parseInt(rp[i]); } catch (Exception ignored) {}
            if (rv > cv) return true;
            if (rv < cv) return false;
        }
        return false;
    }

    private void checkForUpdatesInBackground(final boolean showToastfeedback) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                HttpURLConnection conn = null;
                try {
                    URL url = new URL(VERSION_JSON_URL + "?t=" + System.currentTimeMillis());
                    conn = (HttpURLConnection) url.openConnection();
                    conn.setConnectTimeout(6000);
                    conn.setReadTimeout(6000);
                    conn.setRequestProperty("User-Agent", "WiiU-Transfer-Tools-Android/" + APP_VERSION);
                    if (conn.getResponseCode() == 200) {
                        BufferedReader br = new BufferedReader(new InputStreamReader(conn.getInputStream(), "UTF-8"));
                        StringBuilder sb = new StringBuilder();
                        String line;
                        while ((line = br.readLine()) != null) {
                            sb.append(line);
                        }
                        br.close();

                        JSONObject json = new JSONObject(sb.toString());
                        final String ver = json.optString("version", APP_VERSION);
                        final String apkUrl = json.optString("android_apk_url", DEFAULT_APK_URL);
                        final String wuhbUrl = json.optString("wiiu_wuhb_url", DEFAULT_WUHB_URL);
                        final String notes = json.optString("notes", "");
                        final boolean newer = isRemoteVersionNewer(APP_VERSION, ver);

                        uiHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                latestRemoteVersion = ver;
                                remoteApkUrl = apkUrl;
                                remoteWuhbUrl = wuhbUrl;
                                remoteNotes = notes;
                                updateAvailable = newer;
                                if (updateBannerLayout != null) {
                                    if (newer) {
                                        updateBannerText.setText("\u00a1Nueva versi\u00f3n v" + ver + " disponible en GitHub!");
                                        updateBannerLayout.setVisibility(View.VISIBLE);
                                    } else {
                                        updateBannerLayout.setVisibility(View.GONE);
                                    }
                                }
                                if (showToastfeedback) {
                                    if (newer) {
                                        Toast.makeText(MainActivity.this,
                                                "\u00a1Nueva versi\u00f3n v" + ver + " disponible!", Toast.LENGTH_LONG).show();
                                    } else {
                                        Toast.makeText(MainActivity.this,
                                                "Ya tienes la versi\u00f3n m\u00e1s reciente (v" + APP_VERSION + ").", Toast.LENGTH_SHORT).show();
                                    }
                                }
                            }
                        });
                        return;
                    }
                } catch (Exception ignored) {
                } finally {
                    if (conn != null) conn.disconnect();
                }

                if (showToastfeedback) {
                    uiHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            Toast.makeText(MainActivity.this,
                                    "No se pudo consultar GitHub. Revisa tu conexi\u00f3n a internet.", Toast.LENGTH_SHORT).show();
                        }
                    });
                }
            }
        }, "GitHub-Update-Check").start();
    }

    private void showAboutDialog() {
        final Dialog dlg = new Dialog(this);
        dlg.requestWindowFeature(Window.FEATURE_NO_TITLE);
        if (dlg.getWindow() != null) {
            dlg.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        }

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        card.setBackground(roundedShape(COL_CARD, dp(18), dp(2), COL_CYAN));

        // Encabezado del modal
        LinearLayout topRow = new LinearLayout(this);
        topRow.setOrientation(LinearLayout.HORIZONTAL);
        topRow.setGravity(Gravity.CENTER_VERTICAL);

        ImageView icon = new ImageView(this);
        icon.setImageResource(R.drawable.header_icon);
        LinearLayout.LayoutParams icLp = new LinearLayout.LayoutParams(dp(44), dp(34));
        icLp.rightMargin = dp(10);
        topRow.addView(icon, icLp);

        LinearLayout tCol = new LinearLayout(this);
        tCol.setOrientation(LinearLayout.VERTICAL);
        TextView mTitle = new TextView(this);
        mTitle.setText("Wii U Transfer Tools");
        mTitle.setTextColor(Color.WHITE);
        mTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        mTitle.setTypeface(Typeface.DEFAULT_BOLD);
        tCol.addView(mTitle);

        TextView mVer = new TextView(this);
        mVer.setText("Versi\u00f3n " + APP_VERSION + "  \u2022  Por El Diuca");
        mVer.setTextColor(COL_CYAN_DK);
        mVer.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        mVer.setTypeface(Typeface.DEFAULT_BOLD);
        tCol.addView(mVer);
        topRow.addView(tCol, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(topRow);

        // Descripción breve en lenguaje simple
        TextView descTv = new TextView(this);
        descTv.setText("Herramienta gratuita para transferir e instalar juegos de Wii U desde tu Android, PC o Memoria USB (exFAT, FAT32, NTFS y FAT) por cable USB o Wi-Fi sin pasos t\u00e9cnicos.");
        descTv.setTextColor(COL_TEXT);
        descTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        LinearLayout.LayoutParams descLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        descLp.topMargin = dp(12);
        descLp.bottomMargin = dp(12);
        card.addView(descTv, descLp);

        // Caja Proyecto Oficial en GitHub
        LinearLayout ghBox = new LinearLayout(this);
        ghBox.setOrientation(LinearLayout.VERTICAL);
        ghBox.setPadding(dp(12), dp(10), dp(12), dp(10));
        ghBox.setBackground(roundedShape(COL_BOX_SOFT, dp(12), dp(1), COL_BORDER));

        TextView ghLabel = new TextView(this);
        ghLabel.setText("PROYECTO OFICIAL EN GITHUB");
        ghLabel.setTextColor(COL_MUTED);
        ghLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f);
        ghLabel.setTypeface(Typeface.DEFAULT_BOLD);
        ghBox.addView(ghLabel);

        TextView ghUrlTv = new TextView(this);
        ghUrlTv.setText("github.com/supermafari007/WiiU-Transfer-Tools");
        ghUrlTv.setTextColor(COL_CYAN_DK);
        ghUrlTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        ghUrlTv.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams urlLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        urlLp.topMargin = dp(2);
        urlLp.bottomMargin = dp(8);
        ghBox.addView(ghUrlTv, urlLp);

        TextView btnOpenGh = new TextView(this);
        btnOpenGh.setText("Abrir Repositorio en GitHub");
        btnOpenGh.setTextColor(Color.WHITE);
        btnOpenGh.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        btnOpenGh.setTypeface(Typeface.DEFAULT_BOLD);
        btnOpenGh.setGravity(Gravity.CENTER);
        btnOpenGh.setPadding(dp(10), dp(8), dp(10), dp(8));
        btnOpenGh.setBackground(roundedShape(COL_BTN_OFF, dp(8), dp(1), COL_CYAN));
        btnOpenGh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(GITHUB_REPO_URL)));
                } catch (Exception e) {
                    Toast.makeText(MainActivity.this, GITHUB_REPO_URL, Toast.LENGTH_LONG).show();
                }
            }
        });
        ghBox.addView(btnOpenGh);
        LinearLayout.LayoutParams ghBoxLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        ghBoxLp.bottomMargin = dp(12);
        card.addView(ghBox, ghBoxLp);

        // Caja Centro de Actualizaciones (Android APK + Wii U .wuhb)
        LinearLayout updBox = new LinearLayout(this);
        updBox.setOrientation(LinearLayout.VERTICAL);
        updBox.setPadding(dp(12), dp(10), dp(12), dp(12));
        updBox.setBackground(roundedShape(COL_BOX_SOFT, dp(12), dp(1), updateAvailable ? COL_AMBER_BD : COL_GREEN));

        final TextView updStatusTv = new TextView(this);
        if (updateAvailable) {
            updStatusTv.setText("\u00a1Nueva versi\u00f3n v" + latestRemoteVersion + " disponible para descargar!");
            updStatusTv.setTextColor(COL_AMBER_TX);
        } else {
            updStatusTv.setText("Versi\u00f3n actual al d\u00eda (v" + APP_VERSION + ")  \u2022  Descargas directas:");
            updStatusTv.setTextColor(COL_GREEN_HI);
        }
        updStatusTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        updStatusTv.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams usLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        usLp.bottomMargin = dp(8);
        updBox.addView(updStatusTv, usLp);

        // Botón Actualizar / Descargar APK Android
        TextView btnDlApk = new TextView(this);
        btnDlApk.setText(updateAvailable
                ? "Descargar e Instalar App Android (v" + latestRemoteVersion + ")"
                : "Buscar Actualizaci\u00f3n / Reinstalar APK");
        btnDlApk.setTextColor(Color.WHITE);
        btnDlApk.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        btnDlApk.setTypeface(Typeface.DEFAULT_BOLD);
        btnDlApk.setGravity(Gravity.CENTER);
        btnDlApk.setPadding(dp(10), dp(9), dp(10), dp(9));
        btnDlApk.setBackground(roundedShape(COL_GREEN, dp(8), 0, 0));
        btnDlApk.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                downloadFileFromGitHub(remoteApkUrl, "WiiU_Transfer_Tools_Android.apk", updStatusTv);
            }
        });
        LinearLayout.LayoutParams dlApkLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        dlApkLp.bottomMargin = dp(8);
        updBox.addView(btnDlApk, dlApkLp);

        // Botón Descargar App Wii U (.wuhb)
        TextView btnDlWuhb = new TextView(this);
        btnDlWuhb.setText("Descargar App Wii U (WiiU_Transfer.wuhb)");
        btnDlWuhb.setTextColor(Color.WHITE);
        btnDlWuhb.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        btnDlWuhb.setTypeface(Typeface.DEFAULT_BOLD);
        btnDlWuhb.setGravity(Gravity.CENTER);
        btnDlWuhb.setPadding(dp(10), dp(9), dp(10), dp(9));
        btnDlWuhb.setBackground(roundedShape(COL_CYAN, dp(8), 0, 0));
        btnDlWuhb.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                downloadFileFromGitHub(remoteWuhbUrl, "WiiU_Transfer.wuhb", updStatusTv);
            }
        });
        updBox.addView(btnDlWuhb);

        LinearLayout.LayoutParams updBoxLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        updBoxLp.bottomMargin = dp(12);
        card.addView(updBox, updBoxLp);

        // Botón Cerrar
        TextView btnClose = new TextView(this);
        btnClose.setText("Cerrar");
        btnClose.setTextColor(COL_TEXT);
        btnClose.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        btnClose.setTypeface(Typeface.DEFAULT_BOLD);
        btnClose.setGravity(Gravity.CENTER);
        btnClose.setPadding(dp(12), dp(9), dp(12), dp(9));
        btnClose.setBackground(roundedShape(COL_BTN_OFF, dp(10), dp(1), COL_BORDER));
        btnClose.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                dlg.dismiss();
            }
        });
        card.addView(btnClose);

        dlg.setContentView(card);
        if (dlg.getWindow() != null) {
            int width = (int) (getResources().getDisplayMetrics().widthPixels * 0.92f);
            dlg.getWindow().setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        dlg.show();
        checkForUpdatesInBackground(false);
    }

    private void downloadFileFromGitHub(final String fileUrl, final String outFileName, final TextView statusView) {
        if (statusView != null) {
            statusView.setText("Descargando " + outFileName + " desde GitHub...");
            statusView.setTextColor(COL_CYAN_DK);
        }

        new Thread(new Runnable() {
            @Override
            public void run() {
                File dlDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                if (dlDir == null) {
                    dlDir = new File(Environment.getExternalStorageDirectory(), "Download");
                }
                if (!dlDir.exists()) dlDir.mkdirs();
                final File outFile = outFileName.endsWith(".apk")
                        ? new File(getCacheDir(), outFileName)
                        : new File(dlDir, outFileName);

                HttpURLConnection conn = null;
                boolean ok = false;
                try {
                    URL url = new URL(fileUrl);
                    conn = (HttpURLConnection) url.openConnection();
                    conn.setConnectTimeout(10000);
                    conn.setReadTimeout(20000);
                    conn.setInstanceFollowRedirects(true);
                    conn.setRequestProperty("User-Agent", "WiiU-Transfer-Tools-Android/" + APP_VERSION);
                    if (conn.getResponseCode() == 200) {
                        InputStream is = conn.getInputStream();
                        FileOutputStream fos = new FileOutputStream(outFile);
                        byte[] buf = new byte[64 * 1024];
                        int n;
                        while ((n = is.read(buf)) > 0) {
                            fos.write(buf, 0, n);
                        }
                        fos.close();
                        is.close();
                        ok = true;
                    }
                } catch (Exception ignored) {
                } finally {
                    if (conn != null) conn.disconnect();
                }

                final boolean finalOk = ok;
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (finalOk) {
                            if (outFileName.endsWith(".apk")) {
                                if (statusView != null) {
                                    statusView.setText("Abriendo instalador de actualizaci\u00f3n...");
                                    statusView.setTextColor(COL_GREEN_HI);
                                }
                                launchApkInstaller(outFileName);
                            } else if (outFileName.endsWith(".wuhb")) {
                                MediaScannerConnection.scanFile(MainActivity.this,
                                        new String[]{outFile.getAbsolutePath()}, null, null);
                                pushWuhbToWiiUSd(outFile, statusView);
                            } else {
                                MediaScannerConnection.scanFile(MainActivity.this,
                                        new String[]{outFile.getAbsolutePath()}, null, null);
                                if (statusView != null) {
                                    statusView.setText("Guardado en /Download/" + outFileName);
                                    statusView.setTextColor(COL_GREEN_HI);
                                }
                                Toast.makeText(MainActivity.this,
                                        "Descargado en /Download/" + outFileName, Toast.LENGTH_LONG).show();
                            }
                        } else {
                            if (statusView != null) {
                                statusView.setText("Abriendo descarga en el navegador...");
                                statusView.setTextColor(COL_AMBER_TX);
                            }
                            try {
                                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(fileUrl)));
                            } catch (Exception ignored) {
                            }
                        }
                    }
                });
            }
        }, "GitHub-File-Downloader").start();
    }

    private void launchApkInstaller(String apkName) {
        try {
            if (Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
                Toast.makeText(this, "Activa 'Permitir desde esta fuente' y vuelve a pulsar Instalar.", Toast.LENGTH_LONG).show();
                Intent permIntent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + getPackageName()));
                permIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(permIntent);
                return;
            }
            Uri apkUri = Uri.parse("content://" + ApkFileProvider.AUTHORITY + "/" + apkName);
            Intent installIntent = new Intent(Intent.ACTION_VIEW);
            installIntent.setDataAndType(apkUri, "application/vnd.android.package-archive");
            installIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(installIntent);
        } catch (Exception e) {
            Toast.makeText(this, "No se pudo abrir el instalador: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void pushWuhbToWiiUSd(final File wuhbFile, final TextView statusView) {
        if (statusView != null) {
            statusView.setText("Enviando actualizaci\u00f3n a la SD de tu Wii U...");
            statusView.setTextColor(COL_CYAN_DK);
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                byte[] blob;
                try {
                    FileInputStream fis = new FileInputStream(wuhbFile);
                    java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream((int) wuhbFile.length());
                    byte[] tmp = new byte[64 * 1024];
                    int r;
                    while ((r = fis.read(tmp)) > 0) bos.write(tmp, 0, r);
                    fis.close();
                    blob = bos.toByteArray();
                } catch (Exception e) {
                    return;
                }

                // 1. Si estamos conectados por cable USB directo (AOA), enviar por USB a la SD de Wii U
                if (aoaServer != null && aoaServer.isConnected()) {
                    aoaServer.setPendingWuhbUpdate(blob);
                    uiHandler.post(new Runnable() {
                        @Override
                        public void run() {
                            if (statusView != null) {
                                statusView.setText("Enviando por USB a la SD de tu Wii U (espera 2 seg)...");
                                statusView.setTextColor(COL_GREEN_HI);
                            }
                            Toast.makeText(MainActivity.this,
                                    "Actualizaci\u00f3n enviada por USB a la SD de tu Wii U.", Toast.LENGTH_LONG).show();
                        }
                    });
                    return;
                }

                // 2. Si no hay USB, buscar la consola Wii U en Wi-Fi (UDP 7333) y enviar por TCP 7333
                String wiiuIp = discoverWiiUConsoleIpOnLan();
                if (wiiuIp != null && !wiiuIp.isEmpty()) {
                    boolean pushed = sendWuhbOverTcp7333(wiiuIp, blob);
                    if (pushed) {
                        final String ipOk = wiiuIp;
                        uiHandler.post(new Runnable() {
                            @Override
                            public void run() {
                                if (statusView != null) {
                                    statusView.setText("App Wii U sobrescrita en la SD (" + ipOk + ")!");
                                    statusView.setTextColor(COL_GREEN_HI);
                                }
                                Toast.makeText(MainActivity.this,
                                        "WiiU_Transfer.wuhb actualizado en la SD de tu Wii U!", Toast.LENGTH_LONG).show();
                            }
                        });
                        return;
                    }
                }

                // 3. Fallback: guardado en /Download/WiiU_Transfer.wuhb
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (statusView != null) {
                            statusView.setText("Guardado en /Download/WiiU_Transfer.wuhb (abre la app en Wii U para enviarlo directo)");
                            statusView.setTextColor(COL_GREEN_HI);
                        }
                        Toast.makeText(MainActivity.this,
                                "Guardado en /Download/WiiU_Transfer.wuhb. Conecta tu Wii U para sobrescribir la SD autom\u00e1ticamente.",
                                Toast.LENGTH_LONG).show();
                    }
                });
            }
        }, "WiiU-SD-Push").start();
    }

    private String discoverWiiUConsoleIpOnLan() {
        java.net.DatagramSocket ds = null;
        try {
            ds = new java.net.DatagramSocket();
            ds.setBroadcast(true);
            ds.setSoTimeout(1500);
            byte[] req = "WIIU_FIND_CONSOLE".getBytes("UTF-8");
            java.net.DatagramPacket p = new java.net.DatagramPacket(
                    req, req.length, java.net.InetAddress.getByName("255.255.255.255"), 7333);
            ds.send(p);
            byte[] buf = new byte[128];
            java.net.DatagramPacket resp = new java.net.DatagramPacket(buf, buf.length);
            ds.receive(resp);
            String txt = new String(resp.getData(), 0, resp.getLength(), "UTF-8");
            if (txt.startsWith("WIIU_CONSOLE_HERE")) {
                return resp.getAddress().getHostAddress();
            }
        } catch (Exception ignored) {
        } finally {
            if (ds != null) ds.close();
        }
        if (server != null) {
            String last = server.getFirstClientIp();
            if (last != null && !last.isEmpty()) return last;
        }
        return null;
    }

    private boolean sendWuhbOverTcp7333(String ip, byte[] blob) {
        java.net.Socket s = null;
        try {
            s = new java.net.Socket();
            s.connect(new java.net.InetSocketAddress(ip, 7333), 3000);
            s.setSoTimeout(15000);
            java.io.OutputStream out = s.getOutputStream();
            byte[] hdr = new byte[8];
            hdr[0] = 'W'; hdr[1] = 'U'; hdr[2] = 'P'; hdr[3] = 'U';
            int sz = blob.length;
            hdr[4] = (byte) (sz & 0xFF);
            hdr[5] = (byte) ((sz >> 8) & 0xFF);
            hdr[6] = (byte) ((sz >> 16) & 0xFF);
            hdr[7] = (byte) ((sz >> 24) & 0xFF);
            out.write(hdr);
            out.write(blob);
            out.flush();
            byte[] ack = new byte[16];
            int n = s.getInputStream().read(ack);
            return n >= 2 && ack[0] == 'O' && ack[1] == 'K';
        } catch (Exception e) {
            return false;
        } finally {
            try { if (s != null) s.close(); } catch (Exception ignored) {}
        }
    }

    // =========================================================================
    // Selección de carpeta e importación de juegos (.ZIP)
    // =========================================================================
    private void showChangeFolderDialog() {
        final File extRoot = Environment.getExternalStorageDirectory();
        final File[] presets = new File[]{
                new File(extRoot, "Download"),
                new File(extRoot, "WiiU_Transfer"),
                new File(extRoot, "WUP"),
                new File(extRoot, "install")
        };
        String[] labels = new String[]{
                "Todo Android (Buscar en cualquier carpeta - Recomendado)",
                "Explorar carpetas de Android...",
                "/ almacenamiento / Download (Descargas)",
                "/ almacenamiento / WiiU_Transfer",
                "/ almacenamiento / WUP",
                "/ almacenamiento / install",
                "Escribir otra ruta manualmente..."
        };

        new AlertDialog.Builder(this)
                .setTitle("Elegir ubicaci\u00f3n de juegos")
                .setItems(labels, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if (which == 0) {
                            scanWholePhone = true;
                            markedNames.clear();
                            refreshPackages();
                        } else if (which == 1) {
                            showFolderBrowserDialog(extRoot);
                        } else if (which - 2 < presets.length) {
                            scanWholePhone = false;
                            activeFolder = presets[which - 2];
                            if (!activeFolder.exists()) activeFolder.mkdirs();
                            server.setRootDir(activeFolder);
                            markedNames.clear();
                            refreshPackages();
                        } else {
                            showCustomPathDialog();
                        }
                    }
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void showFolderBrowserDialog(final File currentDir) {
        if (currentDir == null || !currentDir.isDirectory()) return;
        File[] children = currentDir.listFiles();
        final List<File> subDirs = new ArrayList<File>();
        if (children != null) {
            java.util.Arrays.sort(children, new java.util.Comparator<File>() {
                @Override
                public int compare(File a, File b) {
                    return a.getName().compareToIgnoreCase(b.getName());
                }
            });
            for (File c : children) {
                if (c.isDirectory() && !c.getName().startsWith(".")) {
                    subDirs.add(c);
                }
            }
        }

        List<String> items = new ArrayList<String>();
        items.add("\u2713 Usar esta carpeta (" + currentDir.getName() + ")");
        final boolean hasParent = (currentDir.getParentFile() != null && currentDir.getParentFile().canRead());
        if (hasParent) {
            items.add("\u2B06 Subir un nivel (..)");
        }
        for (File d : subDirs) {
            items.add("\uD83D\uDCC1 " + d.getName());
        }

        new AlertDialog.Builder(this)
                .setTitle(currentDir.getAbsolutePath())
                .setItems(items.toArray(new String[0]), new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if (which == 0) {
                            scanWholePhone = false;
                            activeFolder = currentDir;
                            server.setRootDir(activeFolder);
                            markedNames.clear();
                            refreshPackages();
                        } else if (hasParent && which == 1) {
                            showFolderBrowserDialog(currentDir.getParentFile());
                        } else {
                            int idx = which - (hasParent ? 2 : 1);
                            if (idx >= 0 && idx < subDirs.size()) {
                                showFolderBrowserDialog(subDirs.get(idx));
                            }
                        }
                    }
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void showCustomPathDialog() {
        final EditText input = new EditText(this);
        input.setText(activeFolder.getAbsolutePath());
        input.setSingleLine(true);

        new AlertDialog.Builder(this)
                .setTitle("Ruta de la carpeta de juegos")
                .setView(input)
                .setPositiveButton("Guardar", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        String path = input.getText().toString().trim();
                        if (!path.isEmpty()) {
                            File f = new File(path);
                            if (!f.exists()) f.mkdirs();
                            scanWholePhone = false;
                            activeFolder = f;
                            server.setRootDir(activeFolder);
                            markedNames.clear();
                            refreshPackages();
                        }
                    }
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    private void showAddGameDialog() {
        final List<File> zipCandidates = findZipFilesInDownloads();
        List<String> options = new ArrayList<String>();
        options.add("Actualizar lista de juegos (" + activeFolder.getName() + ")");
        options.add("Buscar juegos en /Download");
        for (File z : zipCandidates) {
            options.add("Descomprimir ZIP: " + z.getName() + " (" + NfsaTransferServer.formatBytes(z.length()) + ")");
        }

        new AlertDialog.Builder(this)
                .setTitle("A\u00f1adir juegos a la lista")
                .setItems(options.toArray(new String[0]), new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        if (which == 0) {
                            refreshPackages();
                        } else if (which == 1) {
                            activeFolder = new File(Environment.getExternalStorageDirectory(), "Download");
                            server.setRootDir(activeFolder);
                            markedNames.clear();
                            refreshPackages();
                        } else {
                            File zipFile = zipCandidates.get(which - 2);
                            extractWupZipInBackground(zipFile);
                        }
                    }
                })
                .setNegativeButton("Cerrar", null)
                .show();
    }

    private List<File> findZipFilesInDownloads() {
        List<File> out = new ArrayList<File>();
        File[] dirs = new File[]{
                activeFolder,
                new File(Environment.getExternalStorageDirectory(), "Download")
        };
        for (File d : dirs) {
            if (d == null || !d.exists() || !d.isDirectory()) continue;
            File[] files = d.listFiles();
            if (files == null) continue;
            for (File f : files) {
                if (f.isFile() && f.getName().toLowerCase(Locale.US).endsWith(".zip")) {
                    out.add(f);
                }
            }
        }
        return out;
    }

    private void extractWupZipInBackground(final File zipFile) {
        String baseName = zipFile.getName();
        if (baseName.toLowerCase(Locale.US).endsWith(".zip")) {
            baseName = baseName.substring(0, baseName.length() - 4);
        }
        final File targetDir = new File(activeFolder, baseName);
        bottomSummarySub.setText("Descomprimiendo " + zipFile.getName() + " en " + targetDir.getName() + "...");

        new Thread(new Runnable() {
            @Override
            public void run() {
                boolean ok = false;
                try {
                    if (!targetDir.exists()) targetDir.mkdirs();
                    ZipInputStream zis = new ZipInputStream(new FileInputStream(zipFile));
                    byte[] buf = new byte[128 * 1024];
                    ZipEntry ze;
                    while ((ze = zis.getNextEntry()) != null) {
                        File outFile = new File(targetDir, ze.getName());
                        if (ze.isDirectory()) {
                            outFile.mkdirs();
                        } else {
                            File parent = outFile.getParentFile();
                            if (parent != null && !parent.exists()) parent.mkdirs();
                            FileOutputStream fos = new FileOutputStream(outFile);
                            int n;
                            while ((n = zis.read(buf)) > 0) {
                                fos.write(buf, 0, n);
                            }
                            fos.close();
                        }
                        zis.closeEntry();
                    }
                    zis.close();
                    ok = true;
                } catch (Exception ignored) {
                }

                final boolean finalOk = ok;
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        if (finalOk) {
                            markedNames.add(targetDir.getName());
                            refreshPackages();
                        } else {
                            bottomSummarySub.setText("Error al descomprimir " + zipFile.getName());
                        }
                    }
                });
            }
        }, "WUP-Unzip").start();
    }
}
