/* The frozen workload of the #165 campaign (docs/DECISION_EVIDENCE_PLAN_165.json):
 * one common context, cut by word to 64/256/1024 tokens of each model, and
 * sixteen fixed-option questions. Invented text; latency, not quality. */
#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
static const char context_source[] =
    "The harbour town of Velmora lies at the mouth of the Asel river, where the river meets a shallow bay "
    "protected by two long sand spits. The first written record of the town is a tax roll from the year 1214, "
    "which lists forty-two households, three salt pans and a single wooden pier. For most of its history the "
    "town lived from salt, herring and the river trade that carried timber down from the forests of the upper "
    "valley. The old market square, still paved with grey river stones, was laid out in 1302 after a fire "
    "destroyed most of the wooden houses near the pier. The town council met in the upper room of the weigh "
    "house until 1488, when the present town hall with its square clock tower was completed. The clock itself "
    "was added in 1531 and has been rebuilt twice, most recently in 1897 by a clockmaker from the inland city "
    "of Harnow. In the sixteenth century Velmora joined a league of nine coastal towns that shared the cost of "
    "lighthouses, pilots and a common court for disputes between merchants. The league kept its archive in "
    "Velmora because the town hall had the only vaulted cellar that stayed dry during the spring floods. "
    "Several hundred of its documents survive, among them contracts for salt, insurance letters for cargo and "
    "a long correspondence about the silting of the harbour. Silting was the great problem of the town. Each "
    "winter the river brought down sand and clay, and each summer the channel to the pier became narrower. In "
    "1611 the council hired an engineer named Tobias Reinholt, who proposed a stone breakwater on the northern "
    "spit to turn the current and scour the channel. The breakwater took eleven years to build and cost more "
    "than the town earned from salt in the same period. It worked for about a century. By 1730 the channel had "
    "moved again, and larger ships began to unload at the deeper anchorage of Kessan, a village twelve "
    "kilometres to the south. Velmora declined slowly. The salt pans closed one after another, the last of them "
    "in 1788, and the population fell from about four thousand to fewer than two thousand. The nineteenth "
    "century brought a partial recovery. A railway line from Harnow reached the town in 1871, and a cannery "
    "for herring and sprats opened next to the station two years later. The cannery employed up to three "
    "hundred people, most of them women, and its tins were sold as far away as the southern ports. A new iron "
    "pier, three hundred and twenty metres long, was opened in 1884 so that steamers could tie up at low tide. "
    "At the same time the dunes north of the breakwater became popular with visitors from the inland cities. "
    "The first bathing hotel, the Hotel Asel, opened in 1889 with sixty rooms, a reading hall and a covered "
    "terrace facing the sea. A promenade with a band stand followed in 1902. During the summer months the "
    "number of people in the town doubled, and the railway ran extra trains on Sundays. The twentieth century "
    "was harder. The cannery closed in 1931 after a long dispute about wages and the price of fish. A storm in "
    "February 1953 broke the iron pier in two places and flooded the lower town, including the cellar of the "
    "town hall, where many of the league documents were damaged. Volunteers dried the papers in the hotel "
    "laundry, and a conservator from Harnow spent four years repairing the worst of them. The pier was never "
    "rebuilt in its old length; a shorter concrete pier of one hundred and ten metres replaced it in 1958. "
    "Today Velmora has about five thousand inhabitants. Fishing is still carried out from the concrete pier, "
    "but the main employers are the regional hospital, a school for marine engineering and the summer trade. "
    "The old cannery building houses the town museum, which shows tools from the salt pans, a model of the "
    "breakwater and a selection of league documents in climate-controlled cases. The weigh house is now a "
    "café, and its upper room, where the council once met, can be rented for weddings. Every year on the "
    "first Saturday of June the town holds the Salt Fair, a market that recalls the medieval trade with "
    "stalls, music and a race of small rowing boats across the bay. The race starts at the end of the concrete "
    "pier and finishes at the tip of the breakwater, a distance of about two kilometres. The record time, set "
    "in 2009 by a crew from the marine engineering school, is fourteen minutes and eight seconds. Plans to "
    "deepen the harbour again have been discussed for decades. A study from 2016 estimated that dredging the "
    "old channel and extending the breakwater by two hundred metres would allow small cargo ships to return, "
    "but the cost was judged too high for the town alone. The regional government has instead funded a new "
    "marina for leisure boats on the southern side of the bay, with places for one hundred and forty boats, "
    "which opened in 2021. Environmental groups have asked that the dunes north of the breakwater remain "
    "protected, because they are a breeding ground for terns and other coastal birds. The council has agreed "
    "to close the northern beach between April and July and to limit the number of parking places near the "
    "dunes. Local historians continue to work on the league archive. A digital catalogue of the surviving "
    "documents was published in 2019, listing one thousand two hundred and six items, of which about a third "
    "concern the salt trade, a quarter the harbour and its silting, and the rest disputes, letters and "
    "accounts. The oldest item in the catalogue is a letter from 1347 about the price of herring, written on "
    "parchment and sealed with the green wax that the league used for matters of trade.";
#pragma GCC diagnostic pop

struct source_question {
    const char *id, *question;
    const char *options[4]; /* IDs A..D in order */
};

static const char *const source_option_ids[4] = {"A", "B", "C", "D"};

static const struct source_question source_questions[16] = {
    {"q01", "In which year is the first written record of the town dated?", {"1214", "1302", "1488", "1531"}},
    {"q02", "What was the old market square paved with?", {"Red bricks", "Grey river stones", "Wooden planks",
                                                            "White marble"}},
    {"q03", "Where did the town council meet before the town hall was completed?",
     {"In the church", "In the weigh house", "In the Hotel Asel", "On the pier"}},
    {"q04", "Who rebuilt the clock most recently?", {"A clockmaker from Harnow", "Tobias Reinholt",
                                                      "A crew from the school", "The league of towns"}},
    {"q05", "How many coastal towns belonged to the league?", {"Four", "Seven", "Nine", "Twelve"}},
    {"q06", "Why was the league archive kept in Velmora?",
     {"The town was the largest", "Its cellar stayed dry", "The engineer lived there", "It had a railway"}},
    {"q07", "What did Tobias Reinholt propose?", {"A railway line", "A cannery", "A stone breakwater",
                                                  "A new marina"}},
    {"q08", "Where did larger ships unload after 1730?", {"At Harnow", "At Kessan", "At the iron pier",
                                                         "At the marina"}},
    {"q09", "When did the railway from Harnow reach the town?", {"1788", "1871", "1884", "1902"}},
    {"q10", "How long was the iron pier opened in 1884?", {"110 metres", "200 metres", "320 metres",
                                                          "2 kilometres"}},
    {"q11", "What happened in February 1953?", {"The cannery closed", "A storm broke the pier",
                                                 "The hotel opened", "The marina opened"}},
    {"q12", "What does the old cannery building house today?", {"A café", "The town museum", "A hotel",
                                                               "The marine school"}},
    {"q13", "When is the Salt Fair held?", {"First Saturday of June", "Last Sunday of July", "Every spring flood",
                                            "Each February"}},
    {"q14", "What is the record time of the rowing race?", {"Eight minutes", "Eleven minutes",
                                                            "Fourteen minutes and eight seconds", "Two hours"}},
    {"q15", "How many items does the digital catalogue list?", {"Forty-two", "Three hundred",
                                                                "One thousand two hundred and six", "Five thousand"}},
    {"q16", "What colour of wax did the league use for matters of trade?", {"Red", "Black", "Green", "Blue"}},
};
