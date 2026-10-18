package net.wz2100.warzone2100;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Warzone 2100 entry point on Meta Quest.
 *
 * SDL supplies the Android lifecycle, JNI environment and audio. The game data
 * ships as APK assets and is copied to internal storage whenever its content
 * changes (see data.version, written by build-native.sh), because PhysFS mounts
 * plain files and cannot read AAssets directly. The copy runs on a background
 * thread: blocking onCreate makes Horizon OS background the activity.
 */
public class WarzoneActivity extends SDLActivity
{
    private static final String TAG = "WZ2100";
    private static final String ASSET_DATA_DIR = "data";
    private static final String ASSET_DATA_VERSION = "data.version";

    private Thread mExtractThread;

    private File dataDir()
    {
        return new File(getFilesDir(), "data");
    }

    private File configDir()
    {
        return new File(getFilesDir(), "config");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState)
    {
        configDir().mkdirs();
        mExtractThread = new Thread(this::extractDataIfNeeded, "WZDataExtract");
        mExtractThread.start();
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void main()
    {
        // Runs on SDL's game thread: the game must not start before its data is in place
        if (mExtractThread != null) {
            try {
                mExtractThread.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        super.main();
    }

    @Override
    protected String[] getLibraries()
    {
        // SDL3 and every other dependency are linked statically into libmain.so
        return new String[] { "main" };
    }

    @Override
    protected String[] getArguments()
    {
        return new String[] {
            "--configdir=" + configDir().getAbsolutePath(),
            "--datadir=" + dataDir().getAbsolutePath(),
        };
    }

    private void extractDataIfNeeded()
    {
        String wanted;
        try (InputStream in = getAssets().open(ASSET_DATA_VERSION)) {
            byte[] buf = new byte[128];
            int n = in.read(buf);
            wanted = n > 0 ? new String(buf, 0, n).trim() : "";
        } catch (IOException e) {
            Log.w(TAG, "No " + ASSET_DATA_VERSION + " in assets, always extracting");
            wanted = Long.toString(System.nanoTime());
        }

        File stampFile = new File(getFilesDir(), "data.stamp");
        if (stampFile.exists() && wanted.equals(readSmallFile(stampFile)) && dataDir().isDirectory()) {
            return;
        }

        Log.i(TAG, "Extracting game data (" + wanted + ") to " + dataDir());
        long start = System.currentTimeMillis();
        deleteRecursive(dataDir());
        try {
            copyAssetTree(getAssets(), ASSET_DATA_DIR, dataDir());
            writeSmallFile(stampFile, wanted);
            Log.i(TAG, "Game data extracted in " + (System.currentTimeMillis() - start) + " ms");
        } catch (IOException e) {
            Log.e(TAG, "Failed to extract game data", e);
        }
    }

    private static void copyAssetTree(AssetManager am, String assetPath, File dest) throws IOException
    {
        String[] children = am.list(assetPath);
        if (children == null || children.length == 0) {
            // A file (AssetManager lists files as having no children)
            dest.getParentFile().mkdirs();
            try (InputStream in = am.open(assetPath); OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[1 << 20];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            return;
        }
        dest.mkdirs();
        for (String child : children) {
            copyAssetTree(am, assetPath + "/" + child, new File(dest, child));
        }
    }

    private static void deleteRecursive(File f)
    {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) {
                deleteRecursive(c);
            }
        }
        f.delete();
    }

    private static String readSmallFile(File f)
    {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] buf = new byte[128];
            int n = in.read(buf);
            return n > 0 ? new String(buf, 0, n).trim() : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeSmallFile(File f, String s)
    {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes());
        } catch (IOException e) {
            Log.e(TAG, "Failed to write " + f, e);
        }
    }
}
