// Independent export of pinned pg-starwarsgame-lsp "Show Effective Object" results (P0-05).
//
// Input independence: the only inputs are a .pgproj workspace over the installed corpus, the
// pinned eaw-schema directory and a list of object IDs. The selection loader rejects any document
// carrying EAWR scanner fields, and nothing here reads EAWR output. Every indexed XML buffer is
// hashed from its own bytes before the LSP parses it and again after the exports.
//
// Service graph: the genuine pinned Core/Xml/Schema services, wired as ServerConfigurator does.
// WorkspaceIndexer lives in the server assembly beside asset/model code that needs unpinned
// sibling repositories, so PreScanMetafiles and IndexByLayerAsync are ported below step for step
// (see the comments naming each source method). aet/getEffectiveObject is invoked over an
// in-process OmniSharp JSON-RPC connection against the pinned GetEffectiveObjectHandler.

using System.Collections.Immutable;
using System.IO.Abstractions;
using System.IO.Pipelines;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Microsoft.Extensions.Logging;
using Newtonsoft.Json.Linq;
using OmniSharp.Extensions.JsonRpc;
using PG.StarWarsGame.LSP.Core.Caching;
using PG.StarWarsGame.LSP.Core.Configuration;
using PG.StarWarsGame.LSP.Core.Schema;
using PG.StarWarsGame.LSP.Core.Symbols;
using PG.StarWarsGame.LSP.Core.Util;
using PG.StarWarsGame.LSP.Core.Workspace;
using PG.StarWarsGame.LSP.Schema.Providers;
using PG.StarWarsGame.LSP.Server;
using PG.StarWarsGame.LSP.Server.Project;
using PG.StarWarsGame.LSP.Server.Variants;
using PG.StarWarsGame.LSP.Xml.Parsing;
using PG.StarWarsGame.LSP.Xml.Util;
using PG.StarWarsGame.LSP.Xml.Variants;

namespace Eawr.LspEffectiveExport;

public static class Program
{
    public static async Task<int> Main(string[] args)
    {
        if (args.Length == 3 && args[0] == "--self-test" && args[1] == "--schema")
            return await SelfTest.RunAsync(Path.GetFullPath(args[2]));

        var options = ExportOptions.Parse(args);
        if (options is null)
        {
            Console.Error.WriteLine(
                "usage: LspEffectiveExport --workspace <dir> --schema <eaw-schema>/eaw --selection <json> --out <dir>\n" +
                "       LspEffectiveExport --self-test --schema <eaw-schema>/eaw");
            return 2;
        }

        var report = await Exporter.RunAsync(options);
        await Console.Out.WriteLineAsync(
            $"exported={report.Exported} found={report.Found} render_mismatches={report.RenderMismatches} " +
            $"hash_mismatches={report.HashMismatches} line_unverified={report.LineUnverified}");
        return report.Exported == report.Requested && report.Found == report.Requested
               && report.RenderMismatches == 0 && report.HashMismatches == 0 && report.LineUnverified == 0
            ? 0
            : 1;
    }
}

public sealed record ExportOptions(string Workspace, string SchemaEaw, string Selection, string Out, bool Variants = true)
{
    public static ExportOptions? Parse(string[] args)
    {
        string? workspace = null, schema = null, selection = null, output = null;
        for (var i = 0; i + 1 < args.Length; i += 2)
            switch (args[i])
            {
                case "--workspace": workspace = args[i + 1]; break;
                case "--schema": schema = args[i + 1]; break;
                case "--selection": selection = args[i + 1]; break;
                case "--out": output = args[i + 1]; break;
                default: return null;
            }

        return workspace is null || schema is null || selection is null || output is null || args.Length % 2 != 0
            ? null
            : new ExportOptions(Path.GetFullPath(workspace), Path.GetFullPath(schema), Path.GetFullPath(selection),
                Path.GetFullPath(output));
    }
}

public sealed record ExportReport(int Requested, int Exported, int Found, int RenderMismatches, int HashMismatches,
    int LineUnverified, JsonObject Results);

/// <summary>Reads the object-ID list and refuses anything shaped like an EAWR scanner report.</summary>
public static class SelectionLoader
{
    // Field names only an EAWR xml_scan report or resolver differential would carry.
    private static readonly string[] ForbiddenKeys =
        ["raw_chain", "samples", "effective_values", "input_sha256", "displaced_raw_text", "tags", "registries"];

    public static IReadOnlyList<string> Load(string path)
    {
        var root = JsonNode.Parse(File.ReadAllBytes(path)) as JsonObject
                   ?? throw new InvalidDataException("selection must be a JSON object");
        RejectEawrShape(root, "$");
        var ids = root["object_ids"] as JsonArray
                  ?? throw new InvalidDataException("selection must contain an object_ids array");
        var result = ids.Select(n => n?.GetValue<string>()
                                     ?? throw new InvalidDataException("object_ids entries must be strings")).ToList();
        if (result.Count == 0 || result.Distinct(StringComparer.OrdinalIgnoreCase).Count() != result.Count)
            throw new InvalidDataException("object_ids must be non-empty and unique");
        return result;
    }

    private static void RejectEawrShape(JsonNode? node, string path)
    {
        switch (node)
        {
            case JsonObject obj:
                foreach (var (key, value) in obj)
                {
                    if (ForbiddenKeys.Contains(key, StringComparer.OrdinalIgnoreCase))
                        throw new InvalidDataException(
                            $"selection field {path}.{key} looks like EAWR scanner output; the reference input must be independent");
                    RejectEawrShape(value, $"{path}.{key}");
                }

                break;
            case JsonArray array:
                for (var i = 0; i < array.Count; i++) RejectEawrShape(array[i], $"{path}[{i}]");
                break;
        }
    }
}

public static class Exporter
{
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    public static async Task<ExportReport> RunAsync(ExportOptions options, ILoggerFactory? loggerFactory = null)
    {
        loggerFactory ??= LoggerFactory.Create(b => b.AddSimpleConsole(o => o.SingleLine = true)
            .SetMinimumLevel(LogLevel.Warning));
        var ids = SelectionLoader.Load(options.Selection);
        Directory.CreateDirectory(options.Out);
        var exportDir = Path.Combine(options.Out, "exports");
        Directory.CreateDirectory(exportDir);

        IFileSystem fileSystem = new FileSystem();
        IFileHelper fileHelper = new FileHelper(fileSystem);

        // ── Configuration: the pinned LspConfigurationProvider, fed the VS Code client's default
        // feature object (package.json) with features.tools.variants explicitly enabled.
        var configProvider = new LspConfigurationProvider(fileSystem, loggerFactory.CreateLogger<LspConfigurationProvider>());
        var initOptions = JsonSerializer.SerializeToElement(new
        {
            workspaceRoot = options.Workspace,
            schemaLocalPath = options.SchemaEaw,
            features = ClientFeatureDefaults(options.Variants)
        });
        configProvider.LoadFrom(initOptions);

        // ── Project: ProjectConfigurationResolver.Resolve (detector → loader → resolver).
        var detector = new ModProjectDetector(fileHelper, loggerFactory.CreateLogger<ModProjectDetector>());
        if (!detector.TryFind([options.Workspace], out var pgprojPath) || pgprojPath is null)
            throw new InvalidOperationException($"no .pgproj under {options.Workspace}");
        var loader = new ModProjectLoader(fileHelper, loggerFactory.CreateLogger<ModProjectLoader>());
        var resolver = new ModProjectResolver(fileHelper, loader,
            new ProjectDependencyGraph(loggerFactory.CreateLogger<ProjectDependencyGraph>()),
            loggerFactory.CreateLogger<ModProjectResolver>());
        var config = resolver.Resolve(pgprojPath, loader.Load(pgprojPath));

        // ── Independent input custody: hash every XML buffer under every declared XML directory
        // from its own bytes before any LSP service reads it.
        var preHashes = HashXmlTree(config.XmlDirectories);

        // ── Services, wired as ServerConfigurator/XmlLanguageServiceExtensions register them.
        using var schema = new LocalFileSchemaProvider(options.SchemaEaw, fileSystem,
            loggerFactory.CreateLogger<LocalFileSchemaProvider>());
        var xmlContext = new EaWXmlContext(fileHelper);
        var layerMap = new ProjectLayerMap(fileHelper);
        var fileTypes = new FileTypeRegistry();
        var host = new GameWorkspaceHost(loggerFactory.CreateLogger<GameWorkspaceHost>());
        var textSource = new DocumentTextSource(host, fileHelper, loggerFactory.CreateLogger<DocumentTextSource>());
        var parseCache = new XmlParseCache(textSource, ServerOptions.Default.ParseCacheCapacity,
            loggerFactory.CreateLogger<XmlParseCache>());
        var parser = new XmlGameDocumentParser(fileHelper, schema, fileTypes,
            loggerFactory.CreateLogger<XmlGameDocumentParser>(), parseCache, configProvider, xmlContext);
        var indexService = new GameIndexService(fileHelper, [parser], loggerFactory.CreateLogger<GameIndexService>(),
            layerMap);
        var tagSource = new WorkspaceVariantTagSource(parseCache, indexService);

        // BaselineBootstrapper: the pinned BaselineBuilder cannot be built (unpinned ModVerify engine)
        // and leaves GameObject ObjectTags empty; the default HTTP baseline is unpinned. The index
        // therefore holds BaselineIndex.Empty. ModProjectReloadService.LoadAsync would refuse to index
        // with an empty baseline; this export bypasses only that gate and records it.
        var baselineGate = new JsonObject
        {
            ["baseline"] = "BaselineIndex.Empty",
            ["server_gate"] = "ModProjectReloadService.LoadAsync refuses to index when Baseline.Symbols is empty",
            ["gate_bypassed"] = true
        };

        // ── ModProjectReloadService.LoadAsync order: layers, PreScanMetafiles, IndexDocumentsAsync.
        layerMap.SetLayers(config.Layers);
        var prescan = PreScanMetafiles(config, [options.Workspace], schema, xmlContext, fileTypes, fileHelper,
            configProvider, indexService.Current.Baseline);
        var indexed = await IndexByLayerAsync(config, xmlContext, parser, indexService, fileHelper, preHashes);

        // ── aet/getEffectiveObject over JSON-RPC against the pinned handler.
        var handler = new GetEffectiveObjectHandler(indexService, schema, tagSource, configProvider);
        await using var rpc = await RpcPair.StartAsync(handler);

        var results = new JsonArray();
        int exported = 0, found = 0, renderMismatches = 0, lineUnverified = 0;
        var chainFiles = new SortedSet<string>(StringComparer.Ordinal);
        var lineCache = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);

        foreach (var id in ids)
        {
            var wire = await rpc.GetEffectiveObjectAsync(id);
            var wireResult = wire.ToObject<GetEffectiveObjectResult>()!;
            var index = indexService.Current;

            // The same computation the handler performs, retained with its structured origins.
            var effective = new EffectiveObjectResolver(index, schema, tagSource).Resolve(id);
            var rendered = effective.Found ? EffectiveObjectXmlRenderer.Render(effective) : string.Empty;
            var renderEqual = string.Equals(rendered, wireResult.Xml, StringComparison.Ordinal);
            if (!renderEqual) renderMismatches++;
            if (wireResult.Found) found++;

            var exportName = SafeFileName(id) + ".xml";
            var xmlBytes = Encoding.UTF8.GetBytes(wireResult.Xml);
            await File.WriteAllBytesAsync(Path.Combine(exportDir, exportName), xmlBytes);
            exported++;

            var chain = new JsonArray();
            foreach (var layerId in effective.Chain)
            {
                var winner = index.Resolve(layerId);
                var all = index.ResolveAll(layerId).ToList();
                var winnerRank = winner is null ? -1 : index.LayerRankOf(winner);
                var sameRank = all.Count(s => s.Origin is FileOrigin && index.LayerRankOf(s) == winnerRank);
                var tags = tagSource.TryGetTags(layerId);
                var winnerOrigin = winner?.Origin as FileOrigin;
                if (winnerOrigin is not null) chainFiles.Add(winnerOrigin.Uri);
                chain.Add(new JsonObject
                {
                    ["object_id"] = layerId,
                    ["type_name"] = winner?.TypeName,
                    // Not an LSP field: the harness reads the authored element name from the file at
                    // the LSP-reported origin line, so EAWR's element-name type can be corroborated.
                    ["origin_element_name"] = ElementNameAt(winnerOrigin, layerId, fileHelper, lineCache),
                    ["variant_base_id"] = winner?.VariantBaseId,
                    ["origin"] = OriginJson(winnerOrigin, xmlContext, preHashes, fileHelper),
                    ["layer_rank"] = winnerRank,
                    ["resolved_from"] = winnerOrigin is not null ? "workspace" : winner is null ? "missing" : "baseline",
                    ["workspace_definition_count"] = all.Count(s => s.Origin is FileOrigin),
                    ["same_rank_definition_count"] = sameRank,
                    ["winner_order_dependent"] = sameRank > 1,
                    ["all_definitions"] = new JsonArray(all.Select(s => (JsonNode)new JsonObject
                    {
                        ["origin"] = OriginJson(s.Origin as FileOrigin, xmlContext, preHashes, fileHelper),
                        ["layer_rank"] = index.LayerRankOf(s)
                    }).ToArray()),
                    ["direct_tag_count"] = tags?.Count,
                    ["tag_source"] = tags is null ? "none" : "WorkspaceVariantTagSource"
                });
            }

            var values = new JsonArray();
            var position = 0;
            foreach (var tag in effective.Tags)
            {
                var origin = tag.Origin as FileOrigin;
                var verified = VerifyLine(origin, tag.TagName, fileHelper, lineCache);
                if (verified != true) lineUnverified++;
                values.Add(new JsonObject
                {
                    ["index"] = position++,
                    ["name"] = tag.TagName,
                    ["value"] = tag.Value,
                    ["provenance"] = tag.Provenance.ToString().ToLowerInvariant(),
                    ["source_object_id"] = tag.OriginObjectId,
                    ["origin"] = OriginJson(origin, xmlContext, preHashes, fileHelper),
                    ["origin_line_verified"] = verified,
                    ["base_value"] = tag.BaseValue,
                    ["fragment"] = tag.Fragment,
                    ["fragment_sha256"] = Sha256(Encoding.UTF8.GetBytes(tag.Fragment)),
                    ["fragment_has_child_elements"] = HasChildElements(tag.Fragment)
                });
            }

            results.Add(new JsonObject
            {
                ["requested_id"] = id,
                ["rpc_method"] = "aet/getEffectiveObject",
                ["rpc_result"] = new JsonObject
                {
                    ["found"] = wireResult.Found,
                    ["cyclic"] = wireResult.Cyclic,
                    ["cycle_object_id"] = wireResult.CycleObjectId,
                    ["chain"] = new JsonArray(wireResult.Chain.Select(c => (JsonNode)JsonValue.Create(c)!).ToArray()),
                    ["type_name"] = wireResult.TypeName,
                    ["xml_export"] = "exports/" + exportName,
                    ["xml_sha256"] = Sha256(xmlBytes)
                },
                ["structured_render_equals_rpc_xml"] = renderEqual,
                ["object_id"] = effective.ObjectId,
                ["type_name"] = effective.TypeName,
                ["found"] = effective.Found,
                ["cyclic"] = effective.Cyclic,
                ["chain"] = chain,
                ["values"] = values
            });
        }

        // Re-hash every file that supplied a chain definition: the buffers must not have changed
        // between the independent pre-index hash and the end of the export.
        var hashMismatches = indexed.HashMismatches;
        var inputFiles = new JsonArray();
        foreach (var uri in chainFiles)
        {
            var path = fileHelper.FileUriToPath(uri)!;
            var key = Path.GetFullPath(path);
            var before = preHashes.GetValueOrDefault(key);
            var after = Sha256(await File.ReadAllBytesAsync(path));
            if (before is null || before.Sha256 != after) hashMismatches++;
            inputFiles.Add(new JsonObject
            {
                ["uri"] = uri,
                ["logical_path"] = LogicalPath(uri, xmlContext),
                ["size"] = before?.Size,
                ["sha256_before_index"] = before?.Sha256,
                ["sha256_after_export"] = after,
                ["lsp_project_file_hasher_sha256"] = indexed.LspHashes.GetValueOrDefault(key)
            });
        }

        var manifest = new JsonObject
        {
            ["hashed_by"] = "lsp-effective-export pre-index SHA-256 of original file bytes",
            ["pgproj"] = new JsonObject
            {
                ["path"] = pgprojPath,
                ["sha256"] = Sha256(await File.ReadAllBytesAsync(pgprojPath))
            },
            ["xml_directories"] = new JsonArray(config.XmlDirectories.Select(d => (JsonNode)JsonValue.Create(d)!).ToArray()),
            ["xml_files_hashed"] = preHashes.Count,
            ["xml_files_indexed"] = indexed.Count,
            ["xml_tree_sha256"] = TreeDigest(preHashes),
            ["chain_input_files"] = inputFiles
        };

        var document = new JsonObject
        {
            ["schema_version"] = 1,
            ["producer"] = "tools/inventory/lsp_effective_export",
            ["line_convention"] = "LSP FileOrigin.Line is 0-based (XmlUtility.GetLine = HtmlNode.Line - 1); line1 = line0 + 1",
            ["configuration"] = new JsonObject
            {
                ["features.tools.variants"] = configProvider.Current.Features.Tools.Variants,
                ["features.story.discovery"] = configProvider.Current.Features.Story.Discovery,
                ["features.story.symbols"] = configProvider.Current.Features.Story.Symbols,
                ["schema_source"] = configProvider.Current.SchemaSource.Type.ToString(),
                ["parse_cache_capacity"] = ServerOptions.Default.ParseCacheCapacity
            },
            ["baseline"] = baselineGate,
            ["layers"] = new JsonArray(config.Layers.Select(l => (JsonNode)new JsonObject
            {
                ["rank"] = l.Rank,
                ["name"] = l.Name,
                ["xml_directories"] = new JsonArray(l.XmlDirectories.Select(d => (JsonNode)JsonValue.Create(d)!).ToArray())
            }).ToArray()),
            ["prescan"] = prescan,
            ["indexing"] = new JsonObject
            {
                ["order"] = "sequential, ordinal path order within each layer (the server indexes in parallel)",
                ["files"] = indexed.Count,
                ["documents"] = indexService.Current.Documents.Count,
                ["workspace_definition_ids"] = indexService.Current.WorkspaceDefinitions.Count,
                ["lsp_hash_mismatches"] = indexed.HashMismatches
            },
            ["counts"] = new JsonObject
            {
                ["requested"] = ids.Count,
                ["exported"] = exported,
                ["found"] = found,
                ["render_mismatches"] = renderMismatches,
                ["hash_mismatches"] = hashMismatches,
                ["origin_lines_unverified"] = lineUnverified,
                ["values"] = results.Sum(r => r!["values"]!.AsArray().Count)
            },
            ["results"] = results
        };

        await File.WriteAllTextAsync(Path.Combine(options.Out, "input-manifest.json"),
            manifest.ToJsonString(Indented) + "\n");
        await File.WriteAllTextAsync(Path.Combine(options.Out, "structured-results.json"),
            document.ToJsonString(Indented) + "\n");
        return new ExportReport(ids.Count, exported, found, renderMismatches, hashMismatches, lineUnverified, document);
    }

    // The VS Code client's package.json defaults (aet-eaw-edit.features.*), sent whole as the
    // client does, with tools.variants set explicitly.
    private static object ClientFeatureDefaults(bool variants) => new
    {
        xml = new
        {
            completion = true, hover = true, diagnostics = true, goToDefinition = true, findReferences = true,
            rename = true, codeLens = true, inlayHints = true, codeActions = true, linkedEditing = true,
            autoCloseTag = true
        },
        lua = new
        {
            completion = true, hover = false, diagnostics = false, goToDefinition = true, rename = true,
            codeLens = true, inlayHints = true, codeActions = true, debugger = false
        },
        story = new { discovery = false, graphDiagnostics = false, symbols = false, rename = false },
        dialog = new { diagnostics = false, inlayHints = false, goToDefinition = false, codeActions = false },
        tools = new
        {
            localisation = false, storyEditor = false, storyEditing = false, variants, encyclopedia = true,
            modelPreview = true
        },
        preview = new { energyPool = false }
    };

    // Port of WorkspaceIndexer.PreScanMetafiles (pinned revision), including LocateInLayers,
    // RegisterFromMetafile and FallbackFromBaseline. Story discovery is gated off by the client
    // defaults, exactly as ScanStoryChain would return early.
    private static JsonObject PreScanMetafiles(WorkspaceConfiguration config, IReadOnlyList<string> roots,
        ISchemaProvider schema, EaWXmlContext xmlContext, FileTypeRegistry fileTypes, IFileHelper fileHelper,
        LspConfigurationProvider configProvider, BaselineIndex baseline)
    {
        if (config.XmlDirectories.Count > 0)
            xmlContext.SetDirectories(config.XmlDirectories);
        var leafLayer = config.Layers.Count > 0 ? config.Layers.OrderByDescending(l => l.Rank).First() : null;
        xmlContext.SetLeafDirectories(leafLayer?.XmlDirectories ?? []);

        var xmlRoots = config.XmlDirectories.ToList();
        var records = new JsonArray();
        foreach (var def in schema.AllMetafiles)
        {
            var record = new JsonObject { ["path"] = def.Path, ["type"] = def.MetafileType.ToString() };
            records.Add(record);
            if (def.MetafileType == MetafileType.Special)
            {
                record["outcome"] = configProvider.Current.Features.Story.Discovery
                    ? "story discovery not ported"
                    : "skipped: features.story.discovery=false";
                continue;
            }

            var copies = LocateInLayers(def.Path, roots, xmlRoots, fileHelper);
            if (copies.Count == 0)
            {
                var registered = FallbackFromBaseline(baseline, def, xmlRoots, fileTypes, fileHelper);
                record["outcome"] = $"absent; baseline fallback registered {registered} file(s)";
                continue;
            }

            foreach (var path in copies)
                xmlContext.AddDirectory(fileHelper.FileSystem.Path.GetDirectoryName(path)!);

            var winner = copies[0];
            record["winner"] = winner;
            record["winner_sha256"] = Sha256(File.ReadAllBytes(winner));
            if (def.MetafileType == MetafileType.FileRegistry)
            {
                record["registered_entries"] = RegisterFromMetafile(winner, def, xmlRoots, fileTypes, fileHelper);
                record["outcome"] = "registry";
            }
            else
            {
                fileTypes.RegisterFile(fileHelper.PathToFileUri(winner), def.Types.ToImmutableArray());
                record["outcome"] = "direct content";
            }
        }

        return new JsonObject { ["metafiles"] = records };
    }

    private static List<string> LocateInLayers(string defPath, IReadOnlyList<string> roots,
        IReadOnlyList<string> xmlRoots, IFileHelper fileHelper)
    {
        var found = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        void TryAdd(string? path)
        {
            if (string.IsNullOrEmpty(path) || !fileHelper.FileSystem.File.Exists(path)) return;
            if (seen.Add(fileHelper.NormalizeUri(path)))
                found.Add(path);
        }

        var fileName = fileHelper.FileSystem.Path.GetFileName(defPath);
        foreach (var xmlRoot in xmlRoots.AsEnumerable().Reverse())
            TryAdd(fileHelper.FileSystem.Path.Combine(xmlRoot, fileName));
        TryAdd(fileHelper.FindInWorkspace(roots.ToList(), defPath));
        return found;
    }

    private static int RegisterFromMetafile(string metafilePath, MetafileDefinition def,
        IReadOnlyList<string> xmlRoots, FileTypeRegistry fileTypes, IFileHelper fileHelper)
    {
        XDocument xdoc;
        try
        {
            xdoc = XDocument.Parse(fileHelper.FileSystem.File.ReadAllText(metafilePath));
        }
        catch (Exception)
        {
            return -1; // the server logs and returns: nothing is registered
        }

        var sep = fileHelper.FileSystem.Path.DirectorySeparatorChar;
        var types = def.Types.ToImmutableArray();
        var count = 0;
        foreach (var elem in xdoc.Descendants()
                     .Where(e => e.Name.LocalName.Equals("File", StringComparison.OrdinalIgnoreCase)))
        {
            var filename = elem.Value.Trim();
            if (string.IsNullOrEmpty(filename)) continue;
            var rel = ToXmlRelativePath(fileHelper.NormalizeGamePath(filename)).Replace('/', sep);
            foreach (var xmlRoot in xmlRoots)
                fileTypes.RegisterFile(fileHelper.PathToFileUri(fileHelper.FileSystem.Path.Combine(xmlRoot, rel)), types);
            count++;
        }

        return count;
    }

    private static int FallbackFromBaseline(BaselineIndex baseline, MetafileDefinition def,
        IReadOnlyList<string> xmlRoots, FileTypeRegistry fileTypes, IFileHelper fileHelper)
    {
        if (xmlRoots.Count == 0) return 0;
        var sep = fileHelper.FileSystem.Path.DirectorySeparatorChar;
        var count = 0;
        foreach (var (relativePath, types) in baseline.FileTypeMap)
        {
            if (!types.Any(t => def.Types.Contains(t, StringComparer.OrdinalIgnoreCase))) continue;
            var rel = ToXmlRelativePath(relativePath).Replace('/', sep);
            foreach (var xmlRoot in xmlRoots)
                fileTypes.RegisterFile(fileHelper.PathToFileUri(fileHelper.FileSystem.Path.Combine(xmlRoot, rel)), types);
            count++;
        }

        return count;
    }

    private static string ToXmlRelativePath(string normalizedGamePath)
    {
        const string xmlPrefix = "data/xml/";
        return normalizedGamePath.StartsWith(xmlPrefix, StringComparison.Ordinal)
            ? normalizedGamePath[xmlPrefix.Length..]
            : normalizedGamePath;
    }

    private sealed record IndexOutcome(int Count, int HashMismatches, Dictionary<string, string> LspHashes);

    // Port of WorkspaceIndexer.IndexByLayerAsync + CollectFiles without the project snapshot cache
    // (a cold start). Each file is read and hashed by the pinned ProjectFileHasher and handed to
    // GameIndexService.UpdateDocumentAsync inside one bulk update, as the server does.
    private static async Task<IndexOutcome> IndexByLayerAsync(WorkspaceConfiguration config, EaWXmlContext xmlContext,
        XmlGameDocumentParser parser, GameIndexService indexService, IFileHelper fileHelper,
        IReadOnlyDictionary<string, FileHash> preHashes)
    {
        var count = 0;
        var mismatches = 0;
        var lspHashes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        using (indexService.BeginBulkUpdate())
        {
            foreach (var layer in config.Layers.OrderBy(l => l.Rank))
            {
                var files = layer.XmlDirectories
                    .Where(fileHelper.FileSystem.Directory.Exists)
                    .SelectMany(folder => fileHelper.FileSystem.Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories))
                    .Where(f => fileHelper.FileSystem.Path.GetExtension(f).Equals(".xml", StringComparison.OrdinalIgnoreCase)
                                && parser.CanParse(fileHelper.FileSystem.Path.GetExtension(f))
                                && xmlContext.IsEaWXmlFile(fileHelper.PathToFileUri(f)))
                    .Distinct()
                    .OrderBy(f => f, StringComparer.Ordinal)
                    .ToList();
                foreach (var file in files)
                {
                    var (hash, text) = ProjectFileHasher.ReadAndHash(file, fileHelper.FileSystem);
                    var key = Path.GetFullPath(file);
                    lspHashes[key] = hash;
                    if (!preHashes.TryGetValue(key, out var before) || before.Sha256 != hash) mismatches++;
                    await indexService.UpdateDocumentAsync(fileHelper.PathToFileUri(file), text, 0, CancellationToken.None);
                    count++;
                }
            }
        }

        return new IndexOutcome(count, mismatches, lspHashes);
    }

    public sealed record FileHash(string Sha256, long Size);

    private static Dictionary<string, FileHash> HashXmlTree(IEnumerable<string> directories)
    {
        var result = new Dictionary<string, FileHash>(StringComparer.OrdinalIgnoreCase);
        foreach (var dir in directories.Where(Directory.Exists))
        foreach (var file in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
        {
            if (!file.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)) continue;
            var bytes = File.ReadAllBytes(file);
            result[Path.GetFullPath(file)] = new FileHash(Sha256(bytes), bytes.LongLength);
        }

        return result;
    }

    private static string TreeDigest(IReadOnlyDictionary<string, FileHash> hashes)
    {
        var sb = new StringBuilder();
        foreach (var (path, hash) in hashes.OrderBy(h => h.Key.Replace('\\', '/').ToLowerInvariant(), StringComparer.Ordinal))
            sb.Append(path.Replace('\\', '/').ToLowerInvariant()).Append('\0').Append(hash.Sha256).Append('\n');
        return Sha256(Encoding.UTF8.GetBytes(sb.ToString()));
    }

    private static JsonObject? OriginJson(FileOrigin? origin, EaWXmlContext xmlContext,
        IReadOnlyDictionary<string, FileHash> preHashes, IFileHelper fileHelper)
    {
        if (origin is null) return null;
        var path = fileHelper.FileUriToPath(origin.Uri);
        var hash = path is null ? null : preHashes.GetValueOrDefault(Path.GetFullPath(path));
        return new JsonObject
        {
            ["uri"] = origin.Uri,
            ["logical_path"] = LogicalPath(origin.Uri, xmlContext),
            ["line0"] = origin.Line,
            ["line1"] = origin.Line + 1,
            ["column0"] = origin.Column,
            ["input_sha256"] = hash?.Sha256
        };
    }

    // Game-relative logical path in the EAWR convention (lower-case, forward slashes).
    private static string? LogicalPath(string uri, EaWXmlContext xmlContext)
    {
        var rel = xmlContext.TryGetXmlRelativePath(uri);
        return rel is null ? null : ("data/xml/" + rel).Replace('\\', '/').ToLowerInvariant();
    }

    // Independent check of the 0-based convention: the 1-based source line must open the tag.
    private static bool? VerifyLine(FileOrigin? origin, string tagName, IFileHelper fileHelper,
        Dictionary<string, string[]> cache)
    {
        if (origin is null) return null;
        var path = fileHelper.FileUriToPath(origin.Uri);
        if (path is null) return null;
        if (!cache.TryGetValue(path, out var lines))
            cache[path] = lines = File.ReadAllText(path).Split('\n');
        var line1 = origin.Line + 1;
        if (line1 < 1 || line1 > lines.Length) return false;
        return LineOpensTag(lines[line1 - 1], tagName);
    }

    // True when the line opens an element named exactly tagName: "<Death_Clone" must be followed by
    // whitespace, '>', '/' or the end of the line, so "<Death_Clone_X" does not count.
    internal static bool LineOpensTag(string line, string tagName)
    {
        var open = "<" + tagName;
        for (var at = line.IndexOf(open, StringComparison.OrdinalIgnoreCase); at >= 0;
             at = line.IndexOf(open, at + 1, StringComparison.OrdinalIgnoreCase))
        {
            var next = at + open.Length;
            if (next >= line.Length || char.IsWhiteSpace(line[next]) || line[next] is '>' or '/') return true;
        }

        return false;
    }

    // Authored element name of the object opened on the LSP origin line: the one element on that
    // line whose Name attribute equals the object ID. Null when there is not exactly one.
    private static string? ElementNameAt(FileOrigin? origin, string objectId, IFileHelper fileHelper,
        Dictionary<string, string[]> cache)
    {
        if (origin is null) return null;
        var path = fileHelper.FileUriToPath(origin.Uri);
        if (path is null) return null;
        if (!cache.TryGetValue(path, out var lines))
            cache[path] = lines = File.ReadAllText(path).Split('\n');
        var line1 = origin.Line + 1;
        return line1 < 1 || line1 > lines.Length ? null : ElementNameOnLine(lines[line1 - 1], objectId);
    }

    internal static string? ElementNameOnLine(string line, string objectId)
    {
        var matches = Regex.Matches(line,
            @"<([A-Za-z_][\w.\-]*)(?=[\s/>])[^<>]*?\bName\s*=\s*(?:""([^""]*)""|'([^']*)')");
        var names = matches
            .Where(m => string.Equals(m.Groups[2].Success ? m.Groups[2].Value : m.Groups[3].Value, objectId,
                StringComparison.OrdinalIgnoreCase))
            .Select(m => m.Groups[1].Value)
            .ToList();
        return names.Count == 1 ? names[0] : null;
    }

    private static bool HasChildElements(string fragment)
    {
        var open = fragment.IndexOf('>');
        var close = fragment.LastIndexOf("</", StringComparison.Ordinal);
        if (open < 0 || close <= open) return false;
        var inner = fragment[(open + 1)..close];
        for (var i = 0; i + 1 < inner.Length; i++)
            if (inner[i] == '<' && (char.IsLetter(inner[i + 1]) || inner[i + 1] == '_'))
                return true;
        return false;
    }

    private static string SafeFileName(string id)
    {
        var invalid = Path.GetInvalidFileNameChars();
        return new string(id.Select(c => invalid.Contains(c) ? '_' : c).ToArray());
    }

    public static string Sha256(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
}

/// <summary>An in-process JSON-RPC client/server pair carrying aet/getEffectiveObject.</summary>
public sealed class RpcPair : IAsyncDisposable
{
    private readonly JsonRpcServer _client;
    private readonly JsonRpcServer _server;

    private RpcPair(JsonRpcServer server, JsonRpcServer client)
    {
        _server = server;
        _client = client;
    }

    public static async Task<RpcPair> StartAsync(GetEffectiveObjectHandler handler)
    {
        var toServer = new Pipe();
        var toClient = new Pipe();
        var server = JsonRpcServer.From(o => o.WithInput(toServer.Reader).WithOutput(toClient.Writer).AddHandler(handler));
        var client = JsonRpcServer.From(o => o.WithInput(toClient.Reader).WithOutput(toServer.Writer));
        return new RpcPair(await server, await client);
    }

    public async Task<JToken> GetEffectiveObjectAsync(string objectId)
    {
        return await _client.SendRequest("aet/getEffectiveObject", new GetEffectiveObjectParams { ObjectId = objectId })
            .Returning<JToken>(CancellationToken.None);
    }

    public ValueTask DisposeAsync()
    {
        _client.Dispose();
        _server.Dispose();
        return ValueTask.CompletedTask;
    }
}
