// Synthetic end-to-end checks for the export pipeline. The workspace is authored here, so no
// game content is involved; it needs only the pinned eaw-schema directory.

using System.Text.Json.Nodes;

namespace Eawr.LspEffectiveExport;

public static class SelfTest
{
    private const string Units = """
        <?xml version="1.0" encoding="utf-8"?>
        <Units>
          <SpaceUnit Name="Base_Unit">
            <Tactical_Health>100</Tactical_Health>
            <Death_Clone>Damage_Normal, Base_Clone</Death_Clone>
            <Max_Speed>2</Max_Speed>
          </SpaceUnit>
          <SpaceUnit Name="Mid_Unit">
            <Variant_Of_Existing_Type>Base_Unit</Variant_Of_Existing_Type>
            <Max_Speed>3</Max_Speed>
          </SpaceUnit>
          <SpaceUnit Name="Top_Unit">
            <Variant_Of_Existing_Type>mid_unit</Variant_Of_Existing_Type>
            <Tactical_Health>50</Tactical_Health>
            <Death_Clone>Damage_Fire, Top_Clone</Death_Clone>
            <Added_Tag>x</Added_Tag>
          </SpaceUnit>
        </Units>
        """;

    public static async Task<int> RunAsync(string schemaEaw)
    {
        var failures = new List<string>();
        void Check(bool condition, string message)
        {
            if (!condition) failures.Add(message);
        }

        var root = Path.Combine(Path.GetTempPath(), "eawr-lsp-selftest-" + Guid.NewGuid().ToString("N"));
        try
        {
            var workspace = Path.Combine(root, "workspace");
            var xml = Path.Combine(workspace, "Data", "XML");
            Directory.CreateDirectory(xml);
            await File.WriteAllTextAsync(Path.Combine(workspace, "selftest.pgproj"),
                """{"name": "selftest", "directories": {"xml": ["Data/XML"]}}""");
            await File.WriteAllTextAsync(Path.Combine(xml, "GameObjectFiles.xml"),
                "<Game_Object_Files><File>Units.xml</File></Game_Object_Files>");
            await File.WriteAllTextAsync(Path.Combine(xml, "Units.xml"), Units.Replace("\r\n", "\n"));

            // Input independence: an EAWR-shaped selection is refused before any LSP work starts.
            var eawrShaped = Path.Combine(root, "eawr-shaped.json");
            await File.WriteAllTextAsync(eawrShaped,
                """{"object_ids": ["Top_Unit"], "samples": [{"raw_chain": []}]}""");
            var refused = false;
            try
            {
                SelectionLoader.Load(eawrShaped);
            }
            catch (InvalidDataException)
            {
                refused = true;
            }

            Check(refused, "EAWR-shaped selection was accepted");

            // Line check: an element whose name merely starts with the tag does not open it.
            Check(Exporter.LineOpensTag("  <Death_Clone>a</Death_Clone>", "Death_Clone")
                  && Exporter.LineOpensTag("<Death_Clone Merge=\"Yes\">", "Death_Clone")
                  && Exporter.LineOpensTag("<Death_Clone/>", "Death_Clone")
                  && Exporter.LineOpensTag("  <Death_Clone", "Death_Clone"),
                "line check rejected an element that opens the tag");
            Check(!Exporter.LineOpensTag("  <Death_Clone_X>a</Death_Clone_X>", "Death_Clone")
                  && !Exporter.LineOpensTag("  </Death_Clone>", "Death_Clone"),
                "line check accepted a different element sharing the tag's prefix");
            Check(Exporter.LineOpensTag("<Death_Clone_X/><Death_Clone>b</Death_Clone>", "Death_Clone"),
                "line check stopped at the first prefix match");

            // Authored element name at an origin line: only the element named by the object ID.
            Check(Exporter.ElementNameOnLine("  <SpaceUnit Name=\"A\"><X Name=\"B\"/>", "b") == "X"
                  && Exporter.ElementNameOnLine("<Cin_GroundProp Name='A'>", "A") == "Cin_GroundProp",
                "element name at the origin was not read");
            Check(Exporter.ElementNameOnLine("<SpaceUnit Name=\"AB\">", "A") is null
                  && Exporter.ElementNameOnLine("<SpaceUnit Other_Name=\"A\">", "A") is null
                  && Exporter.ElementNameOnLine("<A Name=\"X\"/><B Name=\"X\"/>", "X") is null,
                "element name read from a non-matching or ambiguous line");

            var selection = Path.Combine(root, "selection.json");
            await File.WriteAllTextAsync(selection, """{"object_ids": ["Top_Unit", "Missing_Unit"]}""");

            var report = await Exporter.RunAsync(
                new ExportOptions(workspace, schemaEaw, selection, Path.Combine(root, "out")));
            var results = report.Results["results"]!.AsArray();
            var top = results[0]!.AsObject();
            var missing = results[1]!.AsObject();

            Check(report.RenderMismatches == 0, "structured render differs from the aet/getEffectiveObject XML");
            Check(report.HashMismatches == 0, "pre-index and LSP/export hashes differ");
            Check(report.LineUnverified == 0, "an origin line did not open its tag");
            Check(top["rpc_result"]!["found"]!.GetValue<bool>(), "Top_Unit not found over RPC");
            Check(!missing["rpc_result"]!["found"]!.GetValue<bool>(), "missing object reported as found");
            Check(Chain(top) == "Top_Unit,Mid_Unit,Base_Unit", $"unexpected chain {Chain(top)}");
            Check(top["chain"]!.AsArray().All(l => Str(l!.AsObject(), "origin_element_name") == "SpaceUnit"),
                "authored element name at the LSP origin was not read");

            var values = top["values"]!.AsArray().Select(v => v!.AsObject()).ToList();
            var names = string.Join(",", values.Select(v => v["name"]!.GetValue<string>()));
            Check(names.StartsWith("Tactical_Health,Death_Clone", StringComparison.Ordinal)
                  && names.EndsWith("Max_Speed,Added_Tag", StringComparison.Ordinal),
                $"first-seen order not preserved: {names}");
            Check(!names.Contains("Variant_Of_Existing_Type", StringComparison.OrdinalIgnoreCase),
                "variant declaration emitted as a value");

            var health = values.Single(v => v["name"]!.GetValue<string>() == "Tactical_Health");
            Check(Str(health, "provenance") == "overridden" && Str(health, "value") == "50"
                                                          && Str(health, "base_value") == "100"
                                                          && Str(health, "source_object_id") == "Top_Unit",
                "Tactical_Health override/provenance/base value wrong");
            Check(health["origin"]!["line1"]!.GetValue<int>() == 14 && health["origin"]!["line0"]!.GetValue<int>() == 13,
                "Tactical_Health origin line normalization wrong");

            var speed = values.Single(v => v["name"]!.GetValue<string>() == "Max_Speed");
            Check(Str(speed, "provenance") == "inherited" && Str(speed, "value") == "3"
                                                        && Str(speed, "source_object_id") == "Mid_Unit"
                                                        && speed["origin"]!["line1"]!.GetValue<int>() == 10,
                "Max_Speed inheritance from the middle layer wrong");

            var added = values.Single(v => v["name"]!.GetValue<string>() == "Added_Tag");
            Check(Str(added, "provenance") == "added" && added["base_value"] is null, "Added_Tag provenance wrong");

            var clones = values.Where(v => v["name"]!.GetValue<string>() == "Death_Clone").ToList();
            Check(clones.Count is 1 or 2, "Death_Clone missing");
            Check(clones.Any(c => Str(c, "provenance") == "merged"), "Death_Clone not merged with its base");

            // Feature gate: with features.tools.variants off the handler answers not-found.
            var gated = await Exporter.RunAsync(
                new ExportOptions(workspace, schemaEaw, selection, Path.Combine(root, "gated"), Variants: false));
            Check(!gated.Results["results"]![0]!["rpc_result"]!["found"]!.GetValue<bool>(),
                "features.tools.variants=false still answered");
        }
        finally
        {
            try
            {
                Directory.Delete(root, true);
            }
            catch (IOException)
            {
            }
        }

        foreach (var failure in failures) await Console.Error.WriteLineAsync("FAIL: " + failure);
        await Console.Out.WriteLineAsync(failures.Count == 0 ? "lsp-effective-export self-test passed" : "self-test failed");
        return failures.Count == 0 ? 0 : 1;
    }

    private static string Chain(JsonObject result) =>
        string.Join(",", result["rpc_result"]!["chain"]!.AsArray().Select(c => c!.GetValue<string>()));

    private static string? Str(JsonObject obj, string key) => obj[key]?.GetValue<string>();
}
