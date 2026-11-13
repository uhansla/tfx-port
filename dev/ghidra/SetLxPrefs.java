import java.util.prefs.Preferences;
public class SetLxPrefs {
    public static void main(String[] a) throws Exception {
        Preferences p = Preferences.userRoot().node("yetmorecode.ghidra.lx.Options");
        p.put("Object base addresses", a[0]);
        p.flush();
        System.out.println("set: " + p.get("Object base addresses", "?"));
    }
}
