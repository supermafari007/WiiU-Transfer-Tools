package com.eldiuca.wiiutransfer;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Motor servidor NFSA (TCP 7331 + Autodescubrimiento UDP 7332) para Android.
 * 100% compatible con WiiU_Transfer.wuhb (protocolo binario NFSA v1).
 */
public class NfsaTransferServer {

    public static final int TCP_PORT = 7331;
    public static final int UDP_PORT = 7332;
    public static final int MAX_READ_CHUNK = 2 * 1024 * 1024;
    public static final int STREAM_CHUNK = 512 * 1024;

    private static final byte[] MAGIC = new byte[]{'N', 'F', 'S', 'A'};
    private static final byte VERSION = 1;

    private static final int CMD_LIST   = 0x01;
    private static final int CMD_STAT   = 0x02;
    private static final int CMD_READ   = 0x03;
    private static final int CMD_CLOSE  = 0x04;
    private static final int CMD_PING   = 0x05;
    private static final int CMD_STREAM = 0x06;

    private static final int STATUS_OK      = 0;
    private static final int STATUS_ERR_BAD = 1;
    private static final int STATUS_ERR_NF  = 2;
    private static final int STATUS_ERR_IO  = 3;

    public interface EventListener {
        void onLog(String level, String message);
    }

    public static class WupPackage {
        public final String name;
        public final File directory;
        public final int fileCount;
        public final long totalBytes;
        public final boolean hasTmd;
        public boolean marked;

        public WupPackage(String name, File directory, int fileCount, long totalBytes, boolean hasTmd, boolean marked) {
            this.name = name;
            this.directory = directory;
            this.fileCount = fileCount;
            this.totalBytes = totalBytes;
            this.hasTmd = hasTmd;
            this.marked = marked;
        }
    }

    private final Object lock = new Object();
    private File rootDir;
    private final Set<String> allowedPackages = new HashSet<String>();
    private final java.util.Map<String, File> packageDirMap = new java.util.LinkedHashMap<String, File>();
    private boolean filterByMarked = false;
    private final EventListener listener;

    private final AtomicBoolean running = new AtomicBoolean(false);
    private ServerSocket tcpServer;
    private DatagramSocket udpSocket;
    private Thread tcpThread;
    private Thread udpThread;

    private final Set<String> activeClients = new HashSet<String>();
    private final AtomicLong bytesSentTotal = new AtomicLong(0);
    private final AtomicLong bytesSentWindow = new AtomicLong(0);
    private String currentStreamingFile = "";

    public NfsaTransferServer(File rootDir, EventListener listener) {
        this.rootDir = rootDir;
        this.listener = listener;
    }

    public void setRootDir(File dir) {
        synchronized (lock) {
            this.rootDir = dir;
        }
        emit("INFO", "Carpeta activa: " + dir.getAbsolutePath());
    }

    public File getRootDir() {
        synchronized (lock) {
            return rootDir;
        }
    }

    public void setMarkedQueue(Set<String> markedNames, boolean onlyServeMarked) {
        synchronized (lock) {
            allowedPackages.clear();
            if (markedNames != null) {
                allowedPackages.addAll(markedNames);
            }
            filterByMarked = onlyServeMarked && !allowedPackages.isEmpty();
        }
    }

    public void setDiscoveredPackages(List<WupPackage> pkgs) {
        synchronized (lock) {
            packageDirMap.clear();
            if (pkgs != null) {
                for (WupPackage p : pkgs) {
                    if (p != null && p.directory != null && p.directory.isDirectory()) {
                        packageDirMap.put(p.name, p.directory);
                    }
                }
            }
        }
    }

    public boolean isRunning() {
        return running.get();
    }

    public String getCurrentStreamingFile() {
        synchronized (lock) {
            return currentStreamingFile;
        }
    }

    public long getBytesSentTotal() {
        return bytesSentTotal.get();
    }

    public long getAndResetWindowBytes() {
        return bytesSentWindow.getAndSet(0);
    }

    public int getActiveClientCount() {
        synchronized (lock) {
            return activeClients.size();
        }
    }

    public String getFirstClientIp() {
        synchronized (lock) {
            if (activeClients.isEmpty()) return "";
            return activeClients.iterator().next();
        }
    }

    private void emit(String level, String msg) {
        if (listener != null) {
            listener.onLog(level, msg);
        }
    }

    public static String formatBytes(long bytes) {
        if (bytes < 1024L) {
            return bytes + " B";
        }
        double kb = bytes / 1024.0;
        if (kb < 1024.0) {
            return String.format(Locale.US, "%.1f KB", kb);
        }
        double mb = kb / 1024.0;
        if (mb < 1024.0) {
            return String.format(Locale.US, "%.1f MB", mb);
        }
        double gb = mb / 1024.0;
        return String.format(Locale.US, "%.2f GB", gb);
    }

    public static String getLocalIpAddress() {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces != null && interfaces.hasMoreElements()) {
                NetworkInterface nif = interfaces.nextElement();
                if (!nif.isUp() || nif.isLoopback()) continue;
                Enumeration<InetAddress> addrs = nif.getInetAddresses();
                while (addrs.hasMoreElements()) {
                    InetAddress addr = addrs.nextElement();
                    if (addr instanceof Inet4Address && !addr.isLoopbackAddress()) {
                        String ip = addr.getHostAddress();
                        if (ip != null && !ip.startsWith("169.254.")) {
                            return ip;
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return "127.0.0.1";
    }

    private static boolean isSystemSkipDir(String name) {
        if (name == null || name.isEmpty() || name.startsWith(".")) return true;
        String low = name.toLowerCase(Locale.US);
        return low.equals("android") || low.equals("dcim") || low.equals("pictures")
                || low.equals("music") || low.equals("movies") || low.equals("notifications")
                || low.equals("ringtones") || low.equals("alarms") || low.equals("podcasts")
                || low.equals("audiobooks") || low.equals("lost.dir")
                || low.equals("system volume information") || low.equals("$recycle.bin");
    }

    private static boolean hasDirectWupFiles(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return false;
        for (File f : files) {
            if (f.isFile()) {
                String low = f.getName().toLowerCase(Locale.US);
                if (low.equals("title.tmd") || low.endsWith(".tmd") || low.endsWith(".tik") || low.endsWith(".app")) {
                    return true;
                }
            }
        }
        return false;
    }

    public static List<WupPackage> scanPackages(File rootDir, Set<String> preserveMarked) {
        List<WupPackage> list = new ArrayList<WupPackage>();
        if (rootDir == null || !rootDir.exists() || !rootDir.isDirectory()) {
            return list;
        }
        Set<String> seenCanon = new HashSet<String>();
        Set<String> usedNames = new HashSet<String>();

        // Si la propia carpeta elegida contiene directamente archivos WUP (.tmd/.tik/.app), incluirla
        if (hasDirectWupFiles(rootDir)) {
            int[] c = new int[]{0};
            long[] sz = new long[]{0L};
            boolean[] tmd = new boolean[]{false};
            scanFolderRecursive(rootDir, 0, c, sz, tmd);
            if (c[0] > 0) {
                String name = rootDir.getName();
                boolean mk = (preserveMarked == null) || preserveMarked.contains(name);
                list.add(new WupPackage(name, rootDir, c[0], sz[0], tmd[0], mk));
                return list;
            }
        }

        findWupDirsRecursive(rootDir, 0, 4, preserveMarked, seenCanon, usedNames, list);
        return list;
    }

    public static List<WupPackage> scanPackagesMultiRoot(List<File> roots, Set<String> preserveMarked) {
        List<WupPackage> list = new ArrayList<WupPackage>();
        Set<String> seenCanon = new HashSet<String>();
        Set<String> usedNames = new HashSet<String>();
        if (roots == null) return list;
        for (File r : roots) {
            if (r == null || !r.exists() || !r.isDirectory()) continue;
            findWupDirsRecursive(r, 0, 4, preserveMarked, seenCanon, usedNames, list);
        }
        return list;
    }

    private static void findWupDirsRecursive(
            File dir, int depth, int maxDepth,
            Set<String> preserveMarked, Set<String> seenCanon,
            Set<String> usedNames, List<WupPackage> outList) {
        if (depth > maxDepth || outList.size() >= 150) return;
        File[] children = dir.listFiles();
        if (children == null) return;

        Arrays.sort(children, new Comparator<File>() {
            @Override
            public int compare(File a, File b) {
                return a.getName().compareToIgnoreCase(b.getName());
            }
        });

        for (File entry : children) {
            if (!entry.isDirectory()) continue;
            String rawName = entry.getName();
            if (isSystemSkipDir(rawName)) continue;

            if (hasDirectWupFiles(entry)) {
                String canon;
                try { canon = entry.getCanonicalPath(); } catch (IOException e) { canon = entry.getAbsolutePath(); }
                if (!seenCanon.add(canon)) continue;

                int[] countHolder = new int[]{0};
                long[] sizeHolder = new long[]{0L};
                boolean[] tmdHolder = new boolean[]{false};
                scanFolderRecursive(entry, 0, countHolder, sizeHolder, tmdHolder);
                if (countHolder[0] > 0) {
                    String uniqueName = rawName;
                    int suffix = 2;
                    while (usedNames.contains(uniqueName.toLowerCase(Locale.US))) {
                        uniqueName = rawName + "_" + suffix++;
                    }
                    usedNames.add(uniqueName.toLowerCase(Locale.US));
                    boolean mk = (preserveMarked == null) || preserveMarked.contains(uniqueName);
                    outList.add(new WupPackage(uniqueName, entry, countHolder[0], sizeHolder[0], tmdHolder[0], mk));
                }
            } else {
                findWupDirsRecursive(entry, depth + 1, maxDepth, preserveMarked, seenCanon, usedNames, outList);
            }
        }
    }

    private static void scanFolderRecursive(File dir, int depth, int[] count, long[] totalSize, boolean[] hasTmd) {
        if (depth > 4) return;
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            String n = f.getName();
            if (n.startsWith(".")) continue;
            if (f.isDirectory()) {
                scanFolderRecursive(f, depth + 1, count, totalSize, hasTmd);
            } else if (f.isFile()) {
                count[0]++;
                totalSize[0] += f.length();
                String low = n.toLowerCase(Locale.US);
                if (low.equals("title.tmd") || low.endsWith(".tmd") || low.endsWith(".app") || low.endsWith(".tik")) {
                    hasTmd[0] = true;
                }
            }
        }
    }

    public void start() throws IOException {
        if (running.get()) return;

        File root = getRootDir();
        if (root != null && !root.exists()) {
            root.mkdirs();
        }

        synchronized (lock) {
            activeClients.clear();
            currentStreamingFile = "";
        }
        bytesSentWindow.set(0);

        tcpServer = new ServerSocket();
        tcpServer.setReuseAddress(true);
        tcpServer.bind(new InetSocketAddress("0.0.0.0", TCP_PORT), 8);
        tcpServer.setSoTimeout(500);

        udpSocket = new DatagramSocket(null);
        udpSocket.setReuseAddress(true);
        udpSocket.setBroadcast(true);
        udpSocket.bind(new InetSocketAddress("0.0.0.0", UDP_PORT));
        udpSocket.setSoTimeout(500);

        running.set(true);

        udpThread = new Thread(new Runnable() {
            @Override
            public void run() {
                udpLoop();
            }
        }, "NFSA-UDP");
        udpThread.setDaemon(true);
        udpThread.start();

        tcpThread = new Thread(new Runnable() {
            @Override
            public void run() {
                tcpLoop();
            }
        }, "NFSA-TCP");
        tcpThread.setDaemon(true);
        tcpThread.start();

        String ip = getLocalIpAddress();
        emit("OK", "Servidor NFSA Android activo en " + ip + ":" + TCP_PORT + " (UDP " + UDP_PORT + ")");
    }

    public void stop() {
        if (!running.getAndSet(false)) return;

        try {
            if (tcpServer != null) tcpServer.close();
        } catch (IOException ignored) {
        }
        try {
            if (udpSocket != null) udpSocket.close();
        } catch (Exception ignored) {
        }
        tcpServer = null;
        udpSocket = null;

        synchronized (lock) {
            activeClients.clear();
            currentStreamingFile = "";
        }
        bytesSentWindow.set(0);
        emit("WARN", "Servidor de red detenido.");
    }

    private void udpLoop() {
        byte[] buf = new byte[256];
        while (running.get() && udpSocket != null && !udpSocket.isClosed()) {
            try {
                DatagramPacket pkt = new DatagramPacket(buf, buf.length);
                udpSocket.receive(pkt);
                String msg = new String(pkt.getData(), pkt.getOffset(), pkt.getLength(), StandardCharsets.UTF_8);
                if (msg.startsWith("NETFSA_DISCOVER")) {
                    String hostModel = android.os.Build.MODEL;
                    if (hostModel == null || hostModel.trim().isEmpty()) {
                        hostModel = "Android-MTP";
                    }
                    hostModel = hostModel.replace('|', '-');
                    byte[] reply = ("NETFSA_HERE|Android-" + hostModel + "|" + TCP_PORT).getBytes(StandardCharsets.UTF_8);
                    DatagramPacket out = new DatagramPacket(reply, reply.length, pkt.getAddress(), pkt.getPort());
                    udpSocket.send(out);
                    emit("INFO", "Wii U descubierta en red: " + pkt.getAddress().getHostAddress());
                }
            } catch (SocketTimeoutException ignored) {
            } catch (Exception e) {
                if (running.get()) {
                    break;
                }
            }
        }
    }

    private void tcpLoop() {
        while (running.get() && tcpServer != null && !tcpServer.isClosed()) {
            try {
                final Socket client = tcpServer.accept();
                Thread t = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        handleClient(client);
                    }
                }, "NFSA-Client");
                t.setDaemon(true);
                t.start();
            } catch (SocketTimeoutException ignored) {
            } catch (IOException e) {
                if (running.get()) {
                    break;
                }
            }
        }
    }

    private void recordSent(long count) {
        bytesSentTotal.addAndGet(count);
        bytesSentWindow.addAndGet(count);
    }

    private static byte[] recvExact(InputStream in, int len, AtomicBoolean runningFlag) throws IOException {
        byte[] out = new byte[len];
        int got = 0;
        while (got < len) {
            if (!runningFlag.get()) {
                throw new IOException("Servidor detenido");
            }
            int n = in.read(out, got, len - got);
            if (n < 0) {
                throw new IOException("Consola desconectada");
            }
            got += n;
        }
        return out;
    }

    private static int sendPacket(OutputStream out, int status, byte[] payload) throws IOException {
        int len = (payload != null) ? payload.length : 0;
        ByteBuffer hdr = ByteBuffer.allocate(12).order(ByteOrder.BIG_ENDIAN);
        hdr.put(MAGIC);
        hdr.put(VERSION);
        hdr.put((byte) status);
        hdr.putShort((short) 0);
        hdr.putInt(len);
        out.write(hdr.array());
        if (len > 0) {
            out.write(payload);
        }
        out.flush();
        return 12 + len;
    }

    private File safeResolve(File root, String relPath) {
        try {
            if (relPath == null) return null;
            String clean = relPath.replace('\\', '/').trim();
            while (clean.startsWith("/")) clean = clean.substring(1);
            if (clean.contains("..")) return null;

            // Buscar primero en el mapa de paquetes descubiertos (cualquier ubicacion del celular)
            if (!clean.isEmpty()) {
                int slash = clean.indexOf('/');
                String top = (slash >= 0) ? clean.substring(0, slash) : clean;
                String rest = (slash >= 0) ? clean.substring(slash + 1) : "";
                File mappedPkgDir;
                synchronized (lock) {
                    mappedPkgDir = packageDirMap.get(top);
                }
                if (mappedPkgDir != null && mappedPkgDir.isDirectory()) {
                    File base = mappedPkgDir.getCanonicalFile();
                    File target = rest.isEmpty() ? base : new File(base, rest).getCanonicalFile();
                    String basePath = base.getPath();
                    String targetPath = target.getPath();
                    if (targetPath.equals(basePath) || targetPath.startsWith(basePath + File.separator)) {
                        return target;
                    }
                    return null;
                }
            }

            if (root == null) return null;
            File base = root.getCanonicalFile();
            File target = new File(base, clean).getCanonicalFile();
            String basePath = base.getPath();
            String targetPath = target.getPath();
            if (!targetPath.equals(basePath) && !targetPath.startsWith(basePath + File.separator)) {
                return null;
            }
            return target;
        } catch (IOException e) {
            return null;
        }
    }

    private void handleClient(Socket conn) {
        String clientIp = conn.getInetAddress().getHostAddress();
        Map<String, RandomAccessFile> openFiles = new HashMap<String, RandomAccessFile>();
        try {
            conn.setSoTimeout(20000);
            conn.setTcpNoDelay(true);
            conn.setKeepAlive(true);
            conn.setSendBufferSize(2 * 1024 * 1024);

            synchronized (lock) {
                activeClients.add(clientIp);
            }
            emit("OK", "Consola Wii U conectada: " + clientIp);

            InputStream in = conn.getInputStream();
            OutputStream out = conn.getOutputStream();
            byte[] streamBuf = new byte[STREAM_CHUNK];

            while (running.get()) {
                byte[] hdrBytes = recvExact(in, 12, running);
                ByteBuffer hdr = ByteBuffer.wrap(hdrBytes).order(ByteOrder.BIG_ENDIAN);
                byte m0 = hdr.get(), m1 = hdr.get(), m2 = hdr.get(), m3 = hdr.get();
                byte ver = hdr.get();
                int cmd = hdr.get() & 0xFF;
                hdr.getShort();
                int payloadLen = hdr.getInt();

                if (m0 != 'N' || m1 != 'F' || m2 != 'S' || m3 != 'A' || ver != VERSION) {
                    recordSent(sendPacket(out, STATUS_ERR_BAD, "Bad header".getBytes(StandardCharsets.UTF_8)));
                    continue;
                }

                byte[] payload = (payloadLen > 0) ? recvExact(in, payloadLen, running) : new byte[0];
                File root = getRootDir();

                if (cmd == CMD_PING) {
                    recordSent(sendPacket(out, STATUS_OK, "PONG".getBytes(StandardCharsets.UTF_8)));
                } else if (cmd == CMD_LIST) {
                    String sub = new String(payload, StandardCharsets.UTF_8).replace("\u0000", "").trim();
                    StringBuilder sb = new StringBuilder();
                    if (sub.isEmpty()) {
                        Set<String> allowedSnap;
                        boolean filterSnap;
                        java.util.Map<String, File> mapSnap;
                        synchronized (lock) {
                            allowedSnap = new HashSet<String>(allowedPackages);
                            filterSnap = filterByMarked;
                            mapSnap = new java.util.LinkedHashMap<String, File>(packageDirMap);
                        }
                        if (!mapSnap.isEmpty()) {
                            for (java.util.Map.Entry<String, File> en : mapSnap.entrySet()) {
                                String pkgName = en.getKey();
                                File dir = en.getValue();
                                if (filterSnap && !allowedSnap.contains(pkgName)) continue;
                                if (dir == null || !dir.isDirectory()) continue;
                                int[] c = new int[]{0};
                                long[] sz = new long[]{0L};
                                boolean[] tmd = new boolean[]{false};
                                scanFolderRecursive(dir, 0, c, sz, tmd);
                                if (c[0] <= 0) continue;
                                if (sb.length() > 0) sb.append('\n');
                                sb.append("D|").append(pkgName).append('|').append(sz[0]).append('|').append(c[0]);
                            }
                        } else {
                            List<WupPackage> pkgs = scanPackages(root, null);
                            for (WupPackage p : pkgs) {
                                if (filterSnap && !allowedSnap.contains(p.name)) continue;
                                if (sb.length() > 0) sb.append('\n');
                                sb.append("D|").append(p.name).append('|').append(p.totalBytes).append('|').append(p.fileCount);
                            }
                        }
                    } else {
                        File target = safeResolve(root, sub);
                        if (target == null || !target.isDirectory()) {
                            recordSent(sendPacket(out, STATUS_ERR_NF, null));
                            continue;
                        }
                        File[] children = target.listFiles();
                        if (children != null) {
                            Arrays.sort(children, new Comparator<File>() {
                                @Override
                                public int compare(File a, File b) {
                                    return a.getName().compareToIgnoreCase(b.getName());
                                }
                            });
                            for (File c : children) {
                                if (c.getName().startsWith(".")) continue;
                                if (sb.length() > 0) sb.append('\n');
                                String kind = c.isDirectory() ? "D" : "F";
                                long sz = c.isFile() ? c.length() : 0L;
                                sb.append(kind).append('|').append(c.getName()).append('|').append(sz).append("|0");
                            }
                        }
                    }
                    recordSent(sendPacket(out, STATUS_OK, sb.toString().getBytes(StandardCharsets.UTF_8)));
                } else if (cmd == CMD_STAT) {
                    String rel = new String(payload, StandardCharsets.UTF_8).replace("\u0000", "").trim();
                    File target = safeResolve(root, rel);
                    if (target == null || !target.exists()) {
                        recordSent(sendPacket(out, STATUS_ERR_NF, null));
                        continue;
                    }
                    ByteBuffer b = ByteBuffer.allocate(9).order(ByteOrder.BIG_ENDIAN);
                    b.put((byte) (target.isDirectory() ? 1 : 0));
                    b.putLong(target.isFile() ? target.length() : 0L);
                    recordSent(sendPacket(out, STATUS_OK, b.array()));
                } else if (cmd == CMD_READ) {
                    if (payload.length < 12) {
                        recordSent(sendPacket(out, STATUS_ERR_BAD, null));
                        continue;
                    }
                    ByteBuffer rb = ByteBuffer.wrap(payload, 0, 12).order(ByteOrder.BIG_ENDIAN);
                    long offset = rb.getLong();
                    int length = Math.min(rb.getInt(), MAX_READ_CHUNK);
                    String rel = new String(payload, 12, payload.length - 12, StandardCharsets.UTF_8).replace("\u0000", "").trim();
                    File target = safeResolve(root, rel);
                    if (target == null || !target.isFile()) {
                        recordSent(sendPacket(out, STATUS_ERR_NF, null));
                        continue;
                    }
                    String key = target.getAbsolutePath();
                    RandomAccessFile raf = openFiles.get(key);
                    if (raf == null) {
                        raf = new RandomAccessFile(target, "r");
                        openFiles.put(key, raf);
                        emit("INFO", "Enviando: " + rel);
                    }
                    raf.seek(offset);
                    long rem = Math.max(0L, raf.length() - offset);
                    int toRead = (int) Math.min((long) length, rem);
                    byte[] data = new byte[toRead];
                    int got = 0;
                    while (got < toRead) {
                        int r = raf.read(data, got, toRead - got);
                        if (r <= 0) break;
                        got += r;
                    }
                    if (got < toRead) {
                        data = Arrays.copyOf(data, got);
                    }
                    recordSent(sendPacket(out, STATUS_OK, data));
                } else if (cmd == CMD_CLOSE) {
                    String rel = new String(payload, StandardCharsets.UTF_8).replace("\u0000", "").trim();
                    File target = safeResolve(root, rel);
                    if (target != null) {
                        RandomAccessFile raf = openFiles.remove(target.getAbsolutePath());
                        if (raf != null) {
                            try {
                                raf.close();
                            } catch (IOException ignored) {
                            }
                        }
                    }
                    recordSent(sendPacket(out, STATUS_OK, null));
                } else if (cmd == CMD_STREAM) {
                    String rel = new String(payload, StandardCharsets.UTF_8).replace("\u0000", "").trim();
                    File target = safeResolve(root, rel);
                    if (target == null || !target.isFile()) {
                        recordSent(sendPacket(out, STATUS_ERR_NF, null));
                        continue;
                    }
                    long fileSize = target.length();
                    synchronized (lock) {
                        currentStreamingFile = rel;
                    }
                    emit("INFO", "Streaming: " + rel + " (" + formatBytes(fileSize) + ")");
                    ByteBuffer sb = ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN);
                    sb.putLong(fileSize);
                    recordSent(sendPacket(out, STATUS_OK, sb.array()));

                    RandomAccessFile raf = new RandomAccessFile(target, "r");
                    try {
                        long remaining = fileSize;
                        while (remaining > 0 && running.get()) {
                            int ask = (int) Math.min((long) streamBuf.length, remaining);
                            int n = raf.read(streamBuf, 0, ask);
                            if (n <= 0) break;
                            out.write(streamBuf, 0, n);
                            remaining -= n;
                            recordSent(n);
                        }
                        out.flush();
                    } finally {
                        try {
                            raf.close();
                        } catch (IOException ignored) {
                        }
                    }
                } else {
                    recordSent(sendPacket(out, STATUS_ERR_BAD, null));
                }
            }
        } catch (Exception e) {
            emit("INFO", "Consola desconectada (" + clientIp + ")");
        } finally {
            for (RandomAccessFile raf : openFiles.values()) {
                try {
                    raf.close();
                } catch (IOException ignored) {
                }
            }
            try {
                conn.close();
            } catch (IOException ignored) {
            }
            synchronized (lock) {
                activeClients.remove(clientIp);
                currentStreamingFile = "";
            }
        }
    }
}
