package com.eldiuca.wiiutransfer;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.ParcelFileDescriptor;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Servidor USB directo por Android Open Accessory (AOA) para Wii U Transfer Tools.
 * Permite conectar el celular a la Wii U por cable USB sin Depuración USB y sin MTP,
 * leyendo directamente los juegos desde la carpeta elegida por el usuario.
 */
public class UsbAoaServer {

    public static final String ACTION_USB_PERMISSION = "com.eldiuca.wiiutransfer.USB_AOA_PERMISSION";

    private static final int MAGIC_REQ  = 0x4F415557; // "WUAO"
    private static final int MAGIC_OK   = 0x4B4F5557; // "WUOK"
    private static final int MAGIC_ERR  = 0x52455557; // "WUER"

    private static final int CMD_PING       = 1;
    private static final int CMD_LIST_PKGS  = 2;
    private static final int CMD_OPEN_FILE  = 3;
    private static final int CMD_READ_CHUNK = 4;
    private static final int CMD_CLOSE_FILE = 5;

    private static final int MAX_AOA_WRITE  = 15 * 1024; // <= 16 KB por escritura USB Bulk AOA
    private static final int MAX_CHUNK_DATA = 12 * 1024; // 12 KB de datos + 12 B header = 12300 B

    public interface Listener {
        void onConnectionChanged(boolean connected, String statusText);
        void onTransferActivity(String currentFile, long totalBytesSent);
    }

    private final Context context;
    private final UsbManager usbManager;
    private final Listener listener;

    private volatile File rootDir;
    private final Set<String> markedNames = new HashSet<String>();
    private final List<File> indexedFiles = new ArrayList<File>();
    private final AtomicBoolean connected = new AtomicBoolean(false);
    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private final AtomicLong totalBytesSent = new AtomicLong(0L);
    private volatile String currentFile = "";

    private ParcelFileDescriptor accessoryPfd;
    private FileInputStream inStream;
    private FileOutputStream outStream;
    private Thread ioThread;
    private boolean receiversRegistered = false;

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            if (intent == null || intent.getAction() == null) return;
            String action = intent.getAction();
            if (ACTION_USB_PERMISSION.equals(action)) {
                UsbAccessory acc = getAccessoryFromIntent(intent);
                boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                if (granted && acc != null) {
                    openAccessory(acc);
                } else {
                    notifyState(false, "Permiso USB denegado. Toca Aceptar al conectar.");
                }
            } else if (UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(action)) {
                UsbAccessory acc = getAccessoryFromIntent(intent);
                if (acc != null) {
                    connectAccessory(acc);
                } else {
                    probeExistingAccessories();
                }
            } else if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(action)) {
                closeAccessory("Cable USB desconectado.");
            }
        }
    };

    public UsbAoaServer(Context context, File rootDir, Listener listener) {
        this.context = context.getApplicationContext();
        this.usbManager = (UsbManager) this.context.getSystemService(Context.USB_SERVICE);
        this.rootDir = rootDir;
        this.listener = listener;
    }

    public void setRootDir(File dir) {
        this.rootDir = dir;
    }

    public synchronized void setMarkedNames(Set<String> names) {
        markedNames.clear();
        if (names != null) {
            markedNames.addAll(names);
        }
    }

    public boolean isConnected() {
        return connected.get();
    }

    public long getTotalBytesSent() {
        return totalBytesSent.get();
    }

    public String getCurrentFile() {
        return currentFile;
    }

    public void register() {
        if (receiversRegistered) return;
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) {
            context.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(usbReceiver, filter);
        }
        receiversRegistered = true;
    }

    public void unregisterAndClose() {
        if (receiversRegistered) {
            try {
                context.unregisterReceiver(usbReceiver);
            } catch (Exception ignored) {
            }
            receiversRegistered = false;
        }
        closeAccessory("Conexi\u00f3n USB cerrada.");
    }

    public void handleIntent(Intent intent) {
        if (intent == null) return;
        if (UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(intent.getAction())) {
            UsbAccessory acc = getAccessoryFromIntent(intent);
            if (acc != null) {
                connectAccessory(acc);
                return;
            }
        }
        probeExistingAccessories();
    }

    public void probeExistingAccessories() {
        if (usbManager == null || connected.get()) return;
        try {
            UsbAccessory[] list = usbManager.getAccessoryList();
            if (list != null && list.length > 0 && list[0] != null) {
                connectAccessory(list[0]);
            }
        } catch (Exception ignored) {
        }
    }

    private UsbAccessory getAccessoryFromIntent(Intent intent) {
        if (intent == null) return null;
        try {
            return (UsbAccessory) intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
        } catch (Exception e) {
            return null;
        }
    }

    private void connectAccessory(UsbAccessory accessory) {
        if (usbManager == null || accessory == null) return;
        if (connected.get()) return;

        if (usbManager.hasPermission(accessory)) {
            openAccessory(accessory);
        } else {
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= 31) {
                flags |= PendingIntent.FLAG_MUTABLE;
            }
            PendingIntent pi = PendingIntent.getBroadcast(
                    context, 0, new Intent(ACTION_USB_PERMISSION).setPackage(context.getPackageName()), flags);
            usbManager.requestPermission(accessory, pi);
            notifyState(false, "Acepta el permiso USB en pantalla para conectar con Wii U.");
        }
    }

    private synchronized void openAccessory(UsbAccessory accessory) {
        if (connected.get()) return;
        closeAccessory(null);

        try {
            ParcelFileDescriptor pfd = usbManager.openAccessory(accessory);
            if (pfd == null) {
                notifyState(false, "No se pudo abrir el canal USB directo con la Wii U.");
                return;
            }
            FileDescriptor fd = pfd.getFileDescriptor();
            accessoryPfd = pfd;
            inStream = new FileInputStream(fd);
            outStream = new FileOutputStream(fd);
            stopping.set(false);
            connected.set(true);
            currentFile = "";
            notifyState(true, "Conectado por USB directo a la Wii U");

            ioThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    runProtocolLoop();
                }
            }, "WiiU-AOA-Server");
            ioThread.start();
        } catch (Exception e) {
            closeAccessory("Error abriendo USB directo: " + e.getMessage());
        }
    }

    public synchronized void closeAccessory(String reason) {
        stopping.set(true);
        boolean wasConn = connected.getAndSet(false);
        currentFile = "";
        try { if (inStream != null) inStream.close(); } catch (Exception ignored) {}
        try { if (outStream != null) outStream.close(); } catch (Exception ignored) {}
        try { if (accessoryPfd != null) accessoryPfd.close(); } catch (Exception ignored) {}
        inStream = null;
        outStream = null;
        accessoryPfd = null;
        if (wasConn && reason != null) {
            notifyState(false, reason);
        }
    }

    private void notifyState(boolean isConn, String msg) {
        if (listener != null) {
            listener.onConnectionChanged(isConn, msg);
        }
    }

    private void runProtocolLoop() {
        byte[] rxBuf = new byte[16 * 1024];
        byte[] accum = new byte[32 * 1024];
        int accumLen = 0;

        RandomAccessFile openRaf = null;
        long openRafSize = 0L;
        byte[] txBuf = new byte[12 + MAX_CHUNK_DATA];

        try {
            while (!stopping.get() && connected.get()) {
                FileInputStream in = inStream;
                if (in == null) break;

                int n = in.read(rxBuf);
                if (n < 0) break;
                if (n == 0) continue;

                if (accumLen + n > accum.length) {
                    accumLen = 0; // resync
                }
                System.arraycopy(rxBuf, 0, accum, accumLen, n);
                accumLen += n;

                while (accumLen >= 12) {
                    int magic = rd32LE(accum, 0);
                    if (magic != MAGIC_REQ) {
                        // Buscar siguiente cabecera WUAO valida
                        int shift = 1;
                        while (shift + 4 <= accumLen && rd32LE(accum, shift) != MAGIC_REQ) {
                            shift++;
                        }
                        System.arraycopy(accum, shift, accum, 0, accumLen - shift);
                        accumLen -= shift;
                        continue;
                    }

                    int cmd = rd32LE(accum, 4);
                    int payloadLen = rd32LE(accum, 8);
                    if (payloadLen < 0 || payloadLen > 16 * 1024) {
                        accumLen = 0;
                        break;
                    }
                    if (accumLen < 12 + payloadLen) {
                        break; // esperar mas bytes
                    }

                    byte[] payload = new byte[payloadLen];
                    if (payloadLen > 0) {
                        System.arraycopy(accum, 12, payload, 0, payloadLen);
                    }
                    int consumed = 12 + payloadLen;
                    if (accumLen > consumed) {
                        System.arraycopy(accum, consumed, accum, 0, accumLen - consumed);
                    }
                    accumLen -= consumed;

                    // Procesar comando
                    if (cmd == CMD_PING) {
                        String model = Build.MANUFACTURER + " " + Build.MODEL;
                        sendFramedResponse(MAGIC_OK, 0, model.getBytes(StandardCharsets.UTF_8));
                    } else if (cmd == CMD_LIST_PKGS) {
                        byte[] manifest = buildPackagesManifest();
                        sendFramedResponse(MAGIC_OK, 0, manifest);
                    } else if (cmd == CMD_OPEN_FILE) {
                        if (openRaf != null) {
                            try { openRaf.close(); } catch (Exception ignored) {}
                            openRaf = null;
                        }
                        File target = null;
                        String label = "";
                        if (payloadLen == 4) {
                            int fileId = rd32LE(payload, 0);
                            synchronized (indexedFiles) {
                                if (fileId >= 0 && fileId < indexedFiles.size()) {
                                    target = indexedFiles.get(fileId);
                                    label = target.getName();
                                }
                            }
                        } else if (payloadLen > 0) {
                            label = new String(payload, StandardCharsets.UTF_8).trim();
                            target = resolveSafeFile(label);
                        }
                        if (target != null && target.isFile()) {
                            try {
                                openRaf = new RandomAccessFile(target, "r");
                                openRafSize = openRaf.length();
                                currentFile = label;
                                if (listener != null) {
                                    listener.onTransferActivity(label, totalBytesSent.get());
                                }
                                byte[] szBytes = new byte[8];
                                wr64LE(szBytes, 0, openRafSize);
                                sendFramedResponse(MAGIC_OK, 0, szBytes);
                            } catch (Exception openEx) {
                                String emsg = "OpenErr: " + openEx.getClass().getSimpleName();
                                if (listener != null) {
                                    listener.onTransferActivity(emsg, totalBytesSent.get());
                                }
                                sendFramedResponse(MAGIC_ERR, 2, emsg.getBytes(StandardCharsets.UTF_8));
                            }
                        } else {
                            String emsg = "NoFile:" + label;
                            if (listener != null) {
                                listener.onTransferActivity(emsg, totalBytesSent.get());
                            }
                            sendFramedResponse(MAGIC_ERR, 1, emsg.getBytes(StandardCharsets.UTF_8));
                        }
                    } else if (cmd == CMD_READ_CHUNK) {
                        if (openRaf == null || payloadLen < 12) {
                            sendFramedResponse(MAGIC_ERR, 2, null);
                        } else {
                            long off = rd64LE(payload, 0);
                            int want = rd32LE(payload, 8);
                            if (want < 0) want = 0;
                            if (want > MAX_CHUNK_DATA) want = MAX_CHUNK_DATA;
                            if (off < 0 || off >= openRafSize) {
                                sendFramedResponse(MAGIC_OK, 0, null);
                            } else {
                                if (openRaf.getFilePointer() != off) {
                                    openRaf.seek(off);
                                }
                                int toRead = (int) Math.min(want, openRafSize - off);
                                int got = 0;
                                while (got < toRead) {
                                    int r = openRaf.read(txBuf, 12 + got, toRead - got);
                                    if (r <= 0) break;
                                    got += r;
                                }
                                wr32LE(txBuf, 0, MAGIC_OK);
                                wr32LE(txBuf, 4, 0);
                                wr32LE(txBuf, 8, got);
                                writeRaw(txBuf, 0, 12 + got);
                                long sent = totalBytesSent.addAndGet(got);
                                if (listener != null && (sent % (256 * 1024) < got)) {
                                    listener.onTransferActivity(currentFile, sent);
                                }
                            }
                        }
                    } else if (cmd == CMD_CLOSE_FILE) {
                        if (openRaf != null) {
                            try { openRaf.close(); } catch (Exception ignored) {}
                            openRaf = null;
                        }
                        currentFile = "";
                        sendFramedResponse(MAGIC_OK, 0, null);
                    } else {
                        sendFramedResponse(MAGIC_ERR, 99, null);
                    }
                }
            }
        } catch (Exception e) {
            // Desconexión USB normal o error de I/O
        } finally {
            if (openRaf != null) {
                try { openRaf.close(); } catch (Exception ignored) {}
            }
            closeAccessory("Conexi\u00f3n USB finalizada.");
        }
    }

    private void sendFramedResponse(int magic, int status, byte[] payload) throws IOException {
        int len = (payload == null) ? 0 : payload.length;
        if (12 + len <= MAX_AOA_WRITE) {
            byte[] pkt = new byte[12 + len];
            wr32LE(pkt, 0, magic);
            wr32LE(pkt, 4, status);
            wr32LE(pkt, 8, len);
            if (len > 0) {
                System.arraycopy(payload, 0, pkt, 12, len);
            }
            writeRaw(pkt, 0, pkt.length);
            return;
        }

        // Respuestas grandes (ej. lista larga de archivos): enviar en bloques <= 12 KB
        byte[] hdr = new byte[12];
        wr32LE(hdr, 0, magic);
        wr32LE(hdr, 4, status);
        wr32LE(hdr, 8, len);
        writeRaw(hdr, 0, 12);

        int off = 0;
        while (off < len) {
            int chunk = Math.min(MAX_CHUNK_DATA, len - off);
            writeRaw(payload, off, chunk);
            off += chunk;
        }
    }

    private void writeRaw(byte[] data, int off, int len) throws IOException {
        FileOutputStream out = outStream;
        if (out == null) throw new IOException("AOA out closed");
        out.write(data, off, len);
        out.flush();
    }

    private File resolveSafeFile(String relPath) {
        File root = rootDir;
        if (root == null || relPath == null || relPath.isEmpty()) return null;
        String clean = relPath.replace('\\', '/');
        if (clean.contains("..")) return null;
        while (clean.startsWith("/")) clean = clean.substring(1);
        File f = new File(root, clean);
        try {
            String rootCanon = root.getCanonicalPath();
            String fCanon = f.getCanonicalPath();
            if (!fCanon.startsWith(rootCanon)) return null;
        } catch (IOException e) {
            return null;
        }
        return f;
    }

    private byte[] buildPackagesManifest() {
        File root = rootDir;
        if (root == null || !root.isDirectory()) return new byte[0];

        Set<String> markedCopy;
        synchronized (this) {
            markedCopy = new HashSet<String>(markedNames);
        }

        File[] children = root.listFiles();
        if (children == null) return new byte[0];
        Arrays.sort(children, new Comparator<File>() {
            @Override
            public int compare(File a, File b) {
                return a.getName().compareToIgnoreCase(b.getName());
            }
        });

        StringBuilder sb = new StringBuilder();
        List<File> newIndex = new ArrayList<File>();
        for (File dir : children) {
            if (!dir.isDirectory() || dir.getName().startsWith(".")) continue;
            List<File> files = new ArrayList<File>();
            boolean[] looksWup = new boolean[]{false};
            collectFiles(dir, 0, files, looksWup);
            if (!looksWup[0] || files.isEmpty()) continue;

            long total = 0L;
            for (File f : files) total += f.length();
            boolean isMarked = markedCopy.contains(dir.getName());

            sb.append("P|").append(dir.getName()).append("|")
              .append(isMarked ? "1" : "0").append("|")
              .append(total).append("\n");

            String basePrefix = dir.getAbsolutePath();
            for (File f : files) {
                int fid = newIndex.size();
                newIndex.add(f);
                String full = f.getAbsolutePath();
                String sub = full.startsWith(basePrefix) ? full.substring(basePrefix.length()) : f.getName();
                sub = sub.replace('\\', '/');
                while (sub.startsWith("/")) sub = sub.substring(1);
                sb.append("F|").append(fid).append("|").append(sub).append("|").append(f.length()).append("\n");
            }
        }
        synchronized (indexedFiles) {
            indexedFiles.clear();
            indexedFiles.addAll(newIndex);
        }
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    private void collectFiles(File dir, int depth, List<File> out, boolean[] looksWup) {
        if (depth > 3) return;
        File[] list = dir.listFiles();
        if (list == null) return;
        Arrays.sort(list, new Comparator<File>() {
            @Override
            public int compare(File a, File b) {
                return a.getName().compareToIgnoreCase(b.getName());
            }
        });
        for (File f : list) {
            if (f.getName().startsWith(".")) continue;
            if (f.isDirectory()) {
                collectFiles(f, depth + 1, out, looksWup);
            } else if (f.isFile()) {
                String low = f.getName().toLowerCase();
                if (low.endsWith(".tmd") || low.endsWith(".tik") || low.endsWith(".app")) {
                    looksWup[0] = true;
                }
                out.add(f);
            }
        }
    }

    private static int rd32LE(byte[] b, int off) {
        return (b[off] & 0xFF)
                | ((b[off + 1] & 0xFF) << 8)
                | ((b[off + 2] & 0xFF) << 16)
                | ((b[off + 3] & 0xFF) << 24);
    }

    private static long rd64LE(byte[] b, int off) {
        long lo = rd32LE(b, off) & 0xFFFFFFFFL;
        long hi = rd32LE(b, off + 4) & 0xFFFFFFFFL;
        return lo | (hi << 32);
    }

    private static void wr32LE(byte[] b, int off, int v) {
        b[off]     = (byte) (v & 0xFF);
        b[off + 1] = (byte) ((v >> 8) & 0xFF);
        b[off + 2] = (byte) ((v >> 16) & 0xFF);
        b[off + 3] = (byte) ((v >> 24) & 0xFF);
    }

    private static void wr64LE(byte[] b, int off, long v) {
        wr32LE(b, off, (int) (v & 0xFFFFFFFFL));
        wr32LE(b, off + 4, (int) ((v >>> 32) & 0xFFFFFFFFL));
    }
}
