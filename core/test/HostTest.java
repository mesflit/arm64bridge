// Desktop test driver: loads the shim exactly like a patched app would
// (System.load of a 64-bit shim named after the 32-bit library).
import com.mesflit.arm64bridge.GuestDemo;

public class HostTest {
    public static void main(String[] args) {
        System.load(args[0]);
        String report = GuestDemo.runAll();
        System.out.println(report);
        System.exit(report.startsWith("PASS") ? 0 : 1);
    }
}
