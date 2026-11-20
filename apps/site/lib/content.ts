export const GITHUB = "https://github.com/apoorvcodes/dreamsy";
export const MCP_COMMAND = "claude mcp add dreamsy -- npx -y dreamsy-mcp";

export type Kit = { id: string; blurb: string; group: "genre" | "foundation" };

export const KITS: Kit[] = [
  { id: "platformer", blurb: "levels, power-ups, bosses", group: "genre" },
  { id: "topdown", blurb: "zelda-like rooms and items", group: "genre" },
  { id: "shooter", blurb: "waves, bullet patterns, shops", group: "genre" },
  { id: "rpg", blurb: "parties, battles, creatures", group: "genre" },
  { id: "farming", blurb: "crops, seasons, neighbours", group: "genre" },
  { id: "puzzle", blurb: "packs, stars, daily puzzles", group: "genre" },
  { id: "racing", blurb: "tracks, ghosts, garages", group: "genre" },
  { id: "runner", blurb: "lanes, missions, obbies", group: "genre" },
  { id: "roguelite", blurb: "seeded runs, relics, meta", group: "genre" },
  { id: "deckbuilder", blurb: "cards, intents, drafting", group: "genre" },
  { id: "story", blurb: "branching scenes, clues", group: "genre" },
  { id: "survival", blurb: "crafting, nights, bases", group: "genre" },
  { id: "simulator", blurb: "collect, sell, rebirth", group: "genre" },
  { id: "tycoon", blurb: "idle income, upgrades", group: "genre" },
  { id: "tower-defense", blurb: "paths, towers, endless", group: "genre" },
  { id: "core", blurb: "events, saves, menus", group: "foundation" },
  { id: "progression", blurb: "quests, skills, achievements", group: "foundation" },
  { id: "juice", blurb: "shake, pops, game feel", group: "foundation" },
  { id: "combat", blurb: "damage, status effects", group: "foundation" },
  { id: "ai", blurb: "enemy behaviours", group: "foundation" },
  { id: "procgen", blurb: "seeded dungeons and maps", group: "foundation" },
];

export type Game = { id: string; name: string; blurb: string; tags: string[]; view: string; featured?: boolean };

export const GAMES: Game[] = [
  { id: "cloud-hopper", name: "Cloud Hopper", blurb: "Pip the explorer hops across floating stone blocks above a sea of clouds.", tags: ["platformer"], view: "2.5D oblique", featured: true },
  { id: "summer-ride", name: "Summer Ride", blurb: "Mio rides her bike along a country road between blue rice fields, cat in the basket.", tags: ["side-scroller"], view: "side", featured: true },
  { id: "kit-farming", name: "Mossbell Farm", blurb: "One spring on a little farm: till, plant, water, ship, and make it to the Blossom Fair.", tags: ["farming"], view: "top-down", featured: true },
  { id: "kit-platformer", name: "Mossling", blurb: "A tiny moss sprite hops through Greenhollow: five levels, hidden gems and a toad boss.", tags: ["platformer"], view: "side", featured: true },
  { id: "street-racer", name: "City Rush", blurb: "Three laps on a looping city highway against five rivals. Hold to boost.", tags: ["racer"], view: "top-down" },
  { id: "fort-raiders", name: "Village Raiders", blurb: "Build a village on a grid, train troops and raid generated outposts.", tags: ["strategy"], view: "top-down" },
  { id: "fox-quest", name: "Fox Explorer", blurb: "A fox runs and jumps through a grassy world full of spikes and gaps.", tags: ["platformer"], view: "side" },
  { id: "kit-rpg", name: "Starfall Keepers", blurb: "Pick a partner, catch star-born creatures, clear Hollow Cave and beat the Starfall Trial.", tags: ["rpg"], view: "top-down" },
  { id: "kit-topdown", name: "Tamsin of Wickhollow", blurb: "Talk to Granny Wick, delve the Mirewell Grotto and beat the Mire Warden.", tags: ["topdown"], view: "top-down" },
  { id: "kit-puzzle", name: "Paper Grid", blurb: "Push crates, link pipes, pop gems. A new flow puzzle every day.", tags: ["puzzle"], view: "flat" },
  { id: "kit-racing", name: "Sprout Speedway", blurb: "Toy cars race through a backyard garden. Beat your own ghost in time trials.", tags: ["racing"], view: "top-down" },
  { id: "kit-runner", name: "Pebble Dash", blurb: "Swipe through three biomes of endless lanes, then climb six obby stages.", tags: ["runner"], view: "side" },
  { id: "kit-shooter", name: "Starling Drift", blurb: "A pocket shmup through a neon nebula, with bullet-hell bosses and a hangar shop.", tags: ["shooter"], view: "top-down" },
  { id: "kit-simulator", name: "Pebble Pals", blurb: "Tap rocks, fill your bag, sell your haul and hatch critter pals.", tags: ["simulator"], view: "flat" },
  { id: "kit-story", name: "Lanternlight", blurb: "The lighthouse went dark the night you arrived. Read the clues, decide who you trust.", tags: ["story"], view: "flat" },
  { id: "kit-survival", name: "Ember Isle", blurb: "Wash up on a wild island, craft your way to iron and hold out through three nights.", tags: ["survival"], view: "top-down" },
  { id: "kit-deckbuilder", name: "Lanternfall", blurb: "Keep the last lamps burning: draft cards, read enemy intents, ring the Drowned Bell.", tags: ["deckbuilder"], view: "flat" },
  { id: "kit-roguelite", name: "Lanternfall Depths", blurb: "Pick a door, clear seeded arenas, stack relic synergies and climb Heat levels.", tags: ["roguelite"], view: "top-down" },
  { id: "kit-tower-defense", name: "Bramble Watch", blurb: "Garden critters guard the seed vault from beetle swarms. Three levels and endless mode.", tags: ["tower-defense"], view: "top-down" },
  { id: "kit-tycoon", name: "Crumbtown Bakery", blurb: "Bake, hire chefs and open eight ovens on your way to the Sugar Moon.", tags: ["tycoon"], view: "flat" },
];
