// Sh4Funcs.java - apply DWARF1 function signatures + source comments.
// optional arg: substring filter on source path (e.g. "sf_chara.c"); empty = all.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import com.google.gson.*;
import java.io.*;
import java.util.*;

public class Sh4Funcs extends GhidraScript {
    DataTypeManager dtm;
    CategoryPath cat = new CategoryPath("/dwarf1");
    Map<Integer, JsonObject> rec = new HashMap<>();
    Map<Integer, DataType> cache = new HashMap<>();
    JsonArray files;
    String filter;
    int nSet, nCreated, nMiss, nErr, nNameClash;

    public void run() throws Exception {
        dtm = currentProgram.getDataTypeManager();
        String[] a = getScriptArgs();
        filter = (a != null && a.length > 0 && !a[0].isEmpty()) ? a[0] : null;
        File modelFile = askModelFile();
        JsonObject root;
        try (Reader r = new BufferedReader(new FileReader(modelFile))) {
            root = JsonParser.parseReader(r).getAsJsonObject();
        }
        JsonObject types = root.getAsJsonObject("types");
        for (Map.Entry<String, JsonElement> e : types.entrySet())
            rec.put(Integer.parseInt(e.getKey()), e.getValue().getAsJsonObject());
        files = root.getAsJsonArray("files");
        JsonArray funcs = root.getAsJsonArray("funcs");
        FunctionManager fm = currentProgram.getFunctionManager();
        println("filter=" + filter + "  funcs in model=" + funcs.size());

        int total = 0;
        for (JsonElement fe : funcs) {
            JsonObject f = fe.getAsJsonObject();
            String file = (f.has("file") && !f.get("file").isJsonNull())
                    ? files.get(f.get("file").getAsInt()).getAsString() : null;
            if (filter != null && (file == null || !file.contains(filter))) continue;
            total++;
            long low = f.get("low").getAsLong();
            String nm = f.get("name").getAsString();
            Address addr = toAddr(low);
            try {
                Function fn = fm.getFunctionAt(addr);
                if (fn == null) { fn = createFunction(addr, nm); if (fn != null) nCreated++; }
                if (fn == null) { nMiss++; continue; }
                try { if (!fn.getName().equals(nm)) fn.setName(nm, SourceType.IMPORTED); }
                catch (Exception ne) { nNameClash++; }
                DataType ret = resolve(f.getAsJsonObject("ret"));
                if (ret == null) ret = VoidDataType.dataType;
                List<ParameterImpl> params = new ArrayList<>();
                int pi = 0;
                for (JsonElement pe : f.getAsJsonArray("params")) {
                    JsonObject p = pe.getAsJsonObject();
                    DataType pt = resolve(p.getAsJsonObject("ref"));
                    if (pt == null || pt.getLength() <= 0) pt = new Undefined4DataType();
                    String pn = p.get("name").getAsString();
                    if (pn == null || pn.isEmpty()) pn = "param_" + (pi + 1);
                    params.add(new ParameterImpl(pn, pt, currentProgram));
                    pi++;
                }
                fn.updateFunction(null,
                        new ReturnParameterImpl(ret, currentProgram), params,
                        Function.FunctionUpdateType.DYNAMIC_STORAGE_FORMAL_PARAMS,
                        true, SourceType.IMPORTED);
                if (file != null) { try { fn.setComment("src: " + file); } catch (Exception ce) {} }
                nSet++;
            } catch (Exception ex) {
                nErr++;
                if (nErr <= 15) println("ERR @" + Long.toHexString(low) + " " + nm + " : " + ex);
            }
        }
        println("matched=" + total + " set=" + nSet + " created=" + nCreated
                + " miss=" + nMiss + " nameClash=" + nNameClash + " err=" + nErr);
    }

    File askModelFile() throws Exception {
        File modelFile = askFile("Select DWARF model.json", "Open");
        if (modelFile == null || !modelFile.isFile())
            throw new FileNotFoundException("Select an existing model.json");
        println("model: " + modelFile.getAbsolutePath());
        return modelFile;
    }

    DataType resolve(JsonObject ref) {
        String k = ref.get("k").getAsString();
        if (k.equals("f")) return fund(ref.get("t").getAsInt());
        if (k.equals("ptr")) return new PointerDataType(resolve(ref.getAsJsonObject("e")), dtm);
        if (k.equals("const") || k.equals("vol")) return resolve(ref.getAsJsonObject("e"));
        if (k.equals("u")) return resolveUdt(ref.get("o").getAsInt());
        return DataType.DEFAULT;
    }
    DataType resolveUdt(int off) {
        if (cache.containsKey(off)) return cache.get(off);
        JsonObject o = rec.get(off);
        if (o == null) return DataType.DEFAULT;
        String kind = o.get("kind").getAsString();
        DataType res;
        if (kind.equals("struct") || kind.equals("union") || kind.equals("enum")) {
            res = dtm.getDataType(cat, uniqueName(o, off));
            if (res == null) res = DataType.DEFAULT;
        } else if (kind.equals("array")) {
            DataType el = resolve(o.getAsJsonObject("ref"));
            if (el == null || el.getLength() <= 0) el = new ByteDataType();
            int cnt = (o.has("count") && !o.get("count").isJsonNull()) ? o.get("count").getAsInt() : 0;
            if (cnt <= 0) cnt = 1;
            res = new ArrayDataType(el, cnt, el.getLength());
        } else if (kind.equals("func")) {
            res = dtm.getDataType(cat, "func_" + Integer.toHexString(off));
            if (res == null) res = new Undefined4DataType();
        } else if (kind.equals("ptr")) {
            res = new PointerDataType(resolve(o.getAsJsonObject("ref")), dtm);
        } else res = DataType.DEFAULT;
        cache.put(off, res);
        return res;
    }
    DataType fund(int t) {
        switch (t) {
            case 0x01: return new CharDataType();
            case 0x02: return new SignedCharDataType();
            case 0x03: return new UnsignedCharDataType();
            case 0x04: case 0x05: return new ShortDataType();
            case 0x06: return new UnsignedShortDataType();
            case 0x07: case 0x08: return new IntegerDataType();
            case 0x09: return new UnsignedIntegerDataType();
            case 0x0a: case 0x0b: return new LongDataType();
            case 0x0c: return new UnsignedLongDataType();
            case 0x0e: return new FloatDataType();
            case 0x0f: return new DoubleDataType();
            case 0x10: return new LongDoubleDataType();
            case 0x14: return VoidDataType.dataType;
            case 0x15: return new BooleanDataType();
            case 0x8008: return new LongLongDataType();
            case 0x8108: return new UnsignedLongLongDataType();
            default: return new Undefined4DataType();
        }
    }
    String rawName(JsonObject o) {
        return (o.has("name") && !o.get("name").isJsonNull()) ? o.get("name").getAsString() : null;
    }
    long sizeOf(JsonObject o) {
        return (o.has("size") && !o.get("size").isJsonNull()) ? o.get("size").getAsLong() : 0;
    }
    String uniqueName(JsonObject o, int off) {
        String nm = rawName(o);
        if (nm == null || nm.startsWith("@")) return "anon_" + Integer.toHexString(off);
        return sanitize(nm);
    }
    String sanitize(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            b.append((Character.isLetterOrDigit(ch) || ch == '_') ? ch : '_');
        }
        String r = b.toString();
        if (r.isEmpty()) r = "t";
        if (Character.isDigit(r.charAt(0))) r = "_" + r;
        return r;
    }
}
