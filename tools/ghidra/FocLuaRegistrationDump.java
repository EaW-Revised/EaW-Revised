// Ghidra script for the FoC debug build. Run with an output TSV path and a names file.
// It follows callers of the Lua member and global registration
// functions, then extracts literal visible names.
//
// The engine's own symbol names never live in this file (clean-room rule, #834). The names
// file (keep it under the ignored out/research/) has one key=value per line, '#' comments:
//   member_function=<name of the member registration function>
//   member_namespace=<namespace that declares it>
//   global_function=<name of the global registration function>
//   global_namespace=<namespace that declares it>
// docs/behaviour/lua-api-declarations.md says how to run it.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.BufferedWriter;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class FocLuaRegistrationDump extends GhidraScript {
    private static final String[] KEYS = {"member_function", "member_namespace", "global_function", "global_namespace"};

    private static Map<String, String> readNames(String path) throws Exception {
        Map<String, String> names = new HashMap<>();
        for (String line : Files.readAllLines(Paths.get(path), StandardCharsets.UTF_8)) {
            String trimmed = line.trim();
            if (trimmed.isEmpty() || trimmed.startsWith("#")) continue;
            int split = trimmed.indexOf('=');
            if (split <= 0) throw new IllegalArgumentException("names file: expected key=value, got: " + trimmed);
            names.put(trimmed.substring(0, split).trim(), trimmed.substring(split + 1).trim());
        }
        for (String key : KEYS) {
            if (!names.containsKey(key) || names.get(key).isEmpty())
                throw new IllegalArgumentException("names file: missing " + key);
        }
        return names;
    }

    // A call of `function` (optionally qualified by `namespace::`) whose Nth argument is a string literal.
    private static Pattern callPattern(String namespace, String function, int literalArgument) {
        StringBuilder regex = new StringBuilder("(?:" + Pattern.quote(namespace + "::") + ")?" + Pattern.quote(function) + "\\s*\\(");
        for (int i = 0; i < literalArgument; ++i) regex.append("[^,]*,\\s*");
        regex.append("\"([^\"]+)\"");
        return Pattern.compile(regex.toString(), Pattern.DOTALL);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) throw new IllegalArgumentException("expected: <output TSV path> <names file>");
        Map<String, String> names = readNames(args[1]);
        String memberFunction = names.get("member_function");
        String memberNamespace = names.get("member_namespace");
        String globalFunction = names.get("global_function");
        String globalNamespace = names.get("global_namespace");
        Pattern member = callPattern(memberNamespace, memberFunction, 1);
        Pattern global = callPattern(globalNamespace, globalFunction, 2);

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        Set<Function> memberCallers = new HashSet<>();
        Set<Function> globalCallers = new HashSet<>();
        for (Function function : currentProgram.getFunctionManager().getFunctions(true)) {
            String name = function.getName();
            String namespace = function.getParentNamespace().getName();
            Set<Function> callers = name.equals(memberFunction) && namespace.equals(memberNamespace) ? memberCallers :
                name.equals(globalFunction) && namespace.equals(globalNamespace) ? globalCallers : null;
            if (callers == null) continue;
            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(function.getEntryPoint());
            while (references.hasNext()) {
                Reference reference = references.next();
                Function caller = currentProgram.getFunctionManager().getFunctionContaining(reference.getFromAddress());
                if (caller != null && !caller.equals(function)) callers.add(caller);
            }
        }
        TreeSet<String> rows = new TreeSet<>();
        Set<Function> allCallers = new HashSet<>(memberCallers);
        allCallers.addAll(globalCallers);
        for (Function function : allCallers) {
            if (monitor.isCancelled()) break;
            DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
            if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
                println("decompile failed: " + function.getName(true));
                continue;
            }
            String code = result.getDecompiledFunction().getC();
            if (memberCallers.contains(function)) {
                Matcher match = member.matcher(code);
                while (match.find()) rows.add("method\t" + function.getParentNamespace().getName() + "\t" + match.group(1));
            }
            if (globalCallers.contains(function)) {
                Matcher match = global.matcher(code);
                while (match.find()) rows.add("global\t\t" + match.group(1));
            }
        }
        try (BufferedWriter out = new BufferedWriter(new FileWriter(args[0]))) {
            for (String row : rows) { out.write(row); out.newLine(); }
        }
        println("FoC Lua registration rows: " + rows.size() + " from " + allCallers.size() + " callers");
        decompiler.dispose();
    }
}
