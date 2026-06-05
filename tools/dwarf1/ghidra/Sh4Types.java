// Sh4Types.java - build DWARF1-recovered types from model.json into /dwarf1
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import com.google.gson.*;
import java.io.*;
import java.util.*;

public class Sh4Types extends GhidraScript {
    DataTypeManager dtm;
    CategoryPath cat = new CategoryPath("/dwarf1");
    Map<Integer, JsonObject> rec = new HashMap<>();
    Map<Integer, DataType> dieToType = new HashMap<>();   // type DIE off -> DataType
    Map<String, Integer> canonOff = new HashMap<>();      // "kind|name" -> canonical off
    Map<Integer, DataType> memo = new HashMap<>();        // on-demand array/func/ptr
    int nStruct, nUnion, nEnum, nSkip, nMemberOk;

    String MODEL = "C:/Users/tux/Projects/sh4xe/tools/dwarf1/model.json";

    public void run() throws Exception {
        dtm = currentProgram.getDataTypeManager();
        JsonObject root;
        try (Reader r = new BufferedReader(new FileReader(MODEL))) {
            root = JsonParser.parseReader(r).getAsJsonObject();
        }
        JsonObject types = root.getAsJsonObject("types");
        for (Map.Entry<String, JsonElement> e : types.entrySet())
            rec.put(Integer.parseInt(e.getKey()), e.getValue().getAsJsonObject());
        println("types indexed: " + rec.size());

        // cleanup previous run
        Category c = dtm.getCategory(cat);
        if (c != null) {
            DataType[] arr = c.getDataTypes();
            for (DataType dt : arr) { try { dtm.remove(dt, monitor); } catch (Exception ex) {} }
            println("cleared previous /dwarf1 types: " + arr.length);
        }

        // pass 1a: canonical offset per real (kind|name) = max(size,memberCount)
        Map<String, long[]> best = new HashMap<>(); // key -> {off, score}
        for (Map.Entry<Integer, JsonObject> e : rec.entrySet()) {
            JsonObject o = e.getValue();
            String kind = o.get("kind").getAsString();
            if (!(kind.equals("struct") || kind.equals("union") || kind.equals("enum"))) continue;
            String nm = rawName(o);
            if (nm == null || nm.startsWith("@")) continue;
            String key = kind + "|" + nm;
            long sz = sizeOf(o);
            long mc = o.has("members") ? o.getAsJsonArray("members").size()
                    : (o.has("consts") ? o.getAsJsonArray("consts").size() : 0);
            long score = sz * 1000 + mc;
            long[] cur = best.get(key);
            if (cur == null || score > cur[1]) best.put(key, new long[]{e.getKey(), score});
        }
        for (Map.Entry<String, long[]> e : best.entrySet())
            canonOff.put(e.getKey(), (int) e.getValue()[0]);

        // collect offsets to materialize: canonical real + every anon
        Set<Integer> mat = new LinkedHashSet<>();
        for (int off : canonOff.values()) mat.add(off);
        for (Map.Entry<Integer, JsonObject> e : rec.entrySet()) {
            JsonObject o = e.getValue();
            String kind = o.get("kind").getAsString();
            if (kind.equals("struct") || kind.equals("union") || kind.equals("enum")) {
                String nm = rawName(o);
                if (nm != null && nm.startsWith("@")) mat.add(e.getKey());
            }
        }
        println("materializing aggregate types: " + mat.size());

        // pass 1b: create shells (enums filled here)
        for (int off : mat) {
            JsonObject o = rec.get(off);
            String kind = o.get("kind").getAsString();
            String nm = uniqueName(o, off);
            DataType dt;
            if (kind.equals("struct")) {
                int sz = (int) Math.max(sizeOf(o), 0);
                dt = new StructureDataType(cat, nm, sz);
            } else if (kind.equals("union")) {
                dt = new UnionDataType(cat, nm);
            } else {
                int bs = (int) sizeOf(o); if (bs != 1 && bs != 2 && bs != 4 && bs != 8) bs = 4;
                EnumDataType en = new EnumDataType(cat, nm, bs);
                if (o.has("consts"))
                    for (JsonElement ce : o.getAsJsonArray("consts")) {
                        JsonArray p = ce.getAsJsonArray();
                        try { en.add(sanitize(p.get(0).getAsString()), p.get(1).getAsLong()); }
                        catch (Exception ex) {}
                    }
                dt = en;
            }
            DataType managed = dtm.addDataType(dt, DataTypeConflictHandler.DEFAULT_HANDLER);
            dieToType.put(off, managed);
        }

        // map non-canonical real-name offsets to their canonical shell
        for (Map.Entry<Integer, JsonObject> e : rec.entrySet()) {
            JsonObject o = e.getValue();
            String kind = o.get("kind").getAsString();
            if (!(kind.equals("struct") || kind.equals("union") || kind.equals("enum"))) continue;
            String nm = rawName(o);
            if (nm == null || nm.startsWith("@")) continue;
            if (dieToType.containsKey(e.getKey())) continue;
            Integer coff = canonOff.get(kind + "|" + nm);
            if (coff != null && dieToType.containsKey(coff)) dieToType.put(e.getKey(), dieToType.get(coff));
        }

        // pass 2: fill struct/union members
        for (int off : mat) {
            JsonObject o = rec.get(off);
            String kind = o.get("kind").getAsString();
            DataType dt = dieToType.get(off);
            if (kind.equals("struct")) {
                Structure s = (Structure) dt;
                for (JsonElement me : o.getAsJsonArray("members")) {
                    JsonObject m = me.getAsJsonObject();
                    int moff = (m.has("off") && !m.get("off").isJsonNull()) ? m.get("off").getAsInt() : -1;
                    DataType mt = resolve(m.getAsJsonObject("ref"));
                    if (mt == null || mt.getLength() <= 0 || moff < 0) { nSkip++; continue; }
                    try { s.replaceAtOffset(moff, mt, mt.getLength(), sanitize(m.get("name").getAsString()), null); nMemberOk++; }
                    catch (Exception ex) { nSkip++; }
                }
                nStruct++;
            } else if (kind.equals("union")) {
                Union u = (Union) dt;
                for (JsonElement me : o.getAsJsonArray("members")) {
                    JsonObject m = me.getAsJsonObject();
                    DataType mt = resolve(m.getAsJsonObject("ref"));
                    if (mt == null || mt.getLength() <= 0) { nSkip++; continue; }
                    try { u.add(mt, sanitize(m.get("name").getAsString()), null); nMemberOk++; }
                    catch (Exception ex) { nSkip++; }
                }
                nUnion++;
            } else nEnum++;
        }

        println("built: struct=" + nStruct + " union=" + nUnion + " enum=" + nEnum
                + " membersOk=" + nMemberOk + " membersSkipped=" + nSkip);
        println("dtm total now: " + dtm.getDataTypeCount(true));
        dumpStruct("sfCharacter");
        dumpStruct("ktLight");
        dumpStruct("sgAnime");
    }

    // ---- helpers ----
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

    DataType resolve(JsonObject ref) {
        String k = ref.get("k").getAsString();
        if (k.equals("f")) return fund(ref.get("t").getAsInt());
        if (k.equals("ptr")) return new PointerDataType(resolve(ref.getAsJsonObject("e")), dtm);
        if (k.equals("const") || k.equals("vol")) return resolve(ref.getAsJsonObject("e"));
        if (k.equals("u")) return resolveUdt(ref.get("o").getAsInt());
        return DataType.DEFAULT;
    }

    DataType resolveUdt(int off) {
        if (dieToType.containsKey(off)) return dieToType.get(off);
        if (memo.containsKey(off)) return memo.get(off);
        JsonObject o = rec.get(off);
        if (o == null) return DataType.DEFAULT;
        String kind = o.get("kind").getAsString();
        if (kind.equals("array")) {
            DataType el = resolve(o.getAsJsonObject("ref"));
            if (el == null || el.getLength() <= 0) el = new ByteDataType();
            int cnt = (o.has("count") && !o.get("count").isJsonNull()) ? o.get("count").getAsInt() : 0;
            if (cnt <= 0) cnt = 1;
            DataType arr = new ArrayDataType(el, cnt, el.getLength());
            memo.put(off, arr); return arr;
        }
        if (kind.equals("func")) {
            FunctionDefinitionDataType fd =
                new FunctionDefinitionDataType(cat, "func_" + Integer.toHexString(off));
            memo.put(off, fd);
            fd.setReturnType(resolve(o.getAsJsonObject("ret")));
            JsonArray ps = o.getAsJsonArray("params");
            ParameterDefinition[] pd = new ParameterDefinition[ps.size()];
            for (int i = 0; i < ps.size(); i++)
                pd[i] = new ParameterDefinitionImpl(null, resolve(ps.get(i).getAsJsonObject()), null);
            try { fd.setArguments(pd); } catch (Exception ex) {}
            DataType m = dtm.addDataType(fd, DataTypeConflictHandler.DEFAULT_HANDLER);
            memo.put(off, m); return m;
        }
        if (kind.equals("ptr")) {
            DataType p = new PointerDataType(resolve(o.getAsJsonObject("ref")), dtm);
            memo.put(off, p); return p;
        }
        return DataType.DEFAULT;
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

    void dumpStruct(String nm) {
        DataType dt = dtm.getDataType(new CategoryPath("/dwarf1"), nm);
        if (!(dt instanceof Structure)) { println("  [dump] " + nm + " not found"); return; }
        Structure s = (Structure) dt;
        println("  struct " + nm + " size=0x" + Integer.toHexString(s.getLength()));
        int n = 0;
        for (DataTypeComponent comp : s.getDefinedComponents()) {
            println("     +0x" + Integer.toHexString(comp.getOffset()) + " "
                    + comp.getDataType().getName() + " " + comp.getFieldName());
            if (++n >= 10) break;
        }
    }
}
