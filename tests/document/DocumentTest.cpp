// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace lcad::document;
using Catch::Matchers::WithinRel;
using lcad::kernel::makeBox;
using lcad::kernel::makeSphere;
using lcad::test::exactPropertyTolerance;

namespace {

std::vector<std::string> names(const Document& document) {
    std::vector<std::string> result;
    for (const Body& body : document.bodies()) {
        result.push_back(body.name);
    }
    return result;
}

} // namespace

TEST_CASE("A new document is empty, unmodified, and has nothing to undo", "[document]") {
    const Document document;

    CHECK(document.bodies().empty());
    CHECK_FALSE(document.isModified());
    CHECK_FALSE(document.canUndo());
    CHECK_FALSE(document.canRedo());
    CHECK(document.undoLabel().empty());
}

TEST_CASE("Bodies can be added, found, renamed, hidden, and removed", "[document]") {
    Document document;

    const ObjectId box = document.addBody("Box", makeBox(10.0, 20.0, 30.0));
    const ObjectId ball = document.addBody("Ball", makeSphere(5.0));

    REQUIRE(box.isValid());
    REQUIRE(ball.isValid());
    CHECK(box != ball);
    CHECK(names(document) == std::vector<std::string>{"Box", "Ball"});
    REQUIRE(document.findBody(box) != nullptr);
    CHECK(document.findBody(box)->visible);
    CHECK_THAT(lcad::kernel::volume(document.findBody(box)->shape),
               WithinRel(6000.0, exactPropertyTolerance));

    document.renameBody(box, "Housing");
    document.setBodyVisible(ball, false);
    CHECK(document.findBody(box)->name == "Housing");
    CHECK_FALSE(document.findBody(ball)->visible);

    document.removeBody(box);
    CHECK(document.findBody(box) == nullptr);
    CHECK(names(document) == std::vector<std::string>{"Ball"});
    CHECK(document.findBody(ObjectId{}) == nullptr);
}

TEST_CASE("An id is never handed out twice", "[document]") {
    Document document;

    const ObjectId first = document.addBody("A", makeBox(1.0, 1.0, 1.0));
    document.removeBody(first);
    const ObjectId second = document.addBody("B", makeBox(1.0, 1.0, 1.0));
    document.undo();
    const ObjectId third = document.addBody("C", makeBox(1.0, 1.0, 1.0));

    CHECK(second != first);
    CHECK(third != first);
    CHECK(third != second);
    CHECK(document.nextObjectId() > third.value);
}

TEST_CASE("Edits that cannot be applied are refused and change nothing", "[document]") {
    Document document;
    const ObjectId box = document.addBody("Box", makeBox(1.0, 2.0, 3.0));
    document.markSaved();
    const ObjectId nobody{999};

    CHECK_THROWS_AS(document.addBody("", makeBox(1.0, 1.0, 1.0)), std::invalid_argument);
    CHECK_THROWS_AS(document.addBody("   ", makeBox(1.0, 1.0, 1.0)), std::invalid_argument);
    CHECK_THROWS_AS(document.addBody("Empty", lcad::kernel::Shape()), std::invalid_argument);
    CHECK_THROWS_AS(document.removeBody(nobody), std::invalid_argument);
    CHECK_THROWS_AS(document.renameBody(nobody, "X"), std::invalid_argument);
    CHECK_THROWS_AS(document.renameBody(box, ""), std::invalid_argument);
    CHECK_THROWS_AS(document.setBodyVisible(nobody, false), std::invalid_argument);

    CHECK(names(document) == std::vector<std::string>{"Box"});
    CHECK_FALSE(document.isModified());
    CHECK(document.undoLabel() == "Add Body");
}

TEST_CASE("Setting a body to the name or visibility it already has is not an edit", "[document]") {
    Document document;
    const ObjectId box = document.addBody("Box", makeBox(1.0, 2.0, 3.0));
    document.markSaved();

    document.renameBody(box, "Box");
    document.setBodyVisible(box, true);

    CHECK_FALSE(document.isModified());
    CHECK(document.undoLabel() == "Add Body");
}

TEST_CASE("Undo and redo step through every kind of edit", "[document][undo]") {
    Document document;
    const ObjectId a = document.addBody("A", makeBox(1.0, 1.0, 1.0));
    const ObjectId b = document.addBody("B", makeBox(2.0, 2.0, 2.0));
    const ObjectId c = document.addBody("C", makeBox(3.0, 3.0, 3.0));
    document.renameBody(b, "Middle");
    document.setBodyVisible(c, false);
    document.removeBody(b);

    REQUIRE(names(document) == std::vector<std::string>{"A", "C"});
    CHECK(document.undoLabel() == "Delete Body");

    document.undo(); // the removal
    // Back in its old place, between the other two, with the name it had when removed.
    CHECK(names(document) == std::vector<std::string>{"A", "Middle", "C"});
    REQUIRE(document.findBody(b) != nullptr);
    CHECK_THAT(lcad::kernel::volume(document.findBody(b)->shape),
               WithinRel(8.0, exactPropertyTolerance));
    CHECK(document.redoLabel() == "Delete Body");
    CHECK(document.undoLabel() == "Hide Body");

    document.undo(); // hiding C
    CHECK(document.findBody(c)->visible);
    CHECK(document.undoLabel() == "Rename Body");

    document.undo(); // the rename
    CHECK(names(document) == std::vector<std::string>{"A", "B", "C"});

    document.undo(); // adding C
    document.undo(); // adding B
    document.undo(); // adding A
    CHECK(document.bodies().empty());
    CHECK_FALSE(document.canUndo());
    CHECK_THROWS_AS(document.undo(), std::logic_error);

    for (int i = 0; i < 6; ++i) {
        REQUIRE(document.canRedo());
        document.redo();
    }
    CHECK(names(document) == std::vector<std::string>{"A", "C"});
    CHECK(document.findBody(a) != nullptr);
    CHECK_FALSE(document.findBody(c)->visible);
    CHECK_FALSE(document.canRedo());
    CHECK_THROWS_AS(document.redo(), std::logic_error);
}

TEST_CASE("A new edit after undo discards what could have been redone", "[document][undo]") {
    Document document;
    document.addBody("A", makeBox(1.0, 1.0, 1.0));
    document.addBody("B", makeBox(1.0, 1.0, 1.0));
    document.undo();
    REQUIRE(document.canRedo());

    document.addBody("C", makeBox(1.0, 1.0, 1.0));

    CHECK_FALSE(document.canRedo());
    CHECK(names(document) == std::vector<std::string>{"A", "C"});
}

TEST_CASE("A committed transaction is one undo step", "[document][transaction]") {
    Document document;
    document.addBody("Before", makeBox(1.0, 1.0, 1.0));

    {
        Transaction transaction(document, "Create Pair");
        const ObjectId left = document.addBody("Left", makeBox(1.0, 1.0, 1.0));
        document.addBody("Right", makeBox(1.0, 1.0, 1.0));
        document.renameBody(left, "Port");
        // Nothing can be undone while the transaction is open.
        CHECK_FALSE(document.canUndo());
        CHECK_THROWS_AS(document.undo(), std::logic_error);
        transaction.commit();
    }

    CHECK(names(document) == std::vector<std::string>{"Before", "Port", "Right"});
    CHECK(document.undoLabel() == "Create Pair");

    document.undo();
    CHECK(names(document) == std::vector<std::string>{"Before"});
    CHECK(document.undoLabel() == "Add Body");

    document.redo();
    CHECK(names(document) == std::vector<std::string>{"Before", "Port", "Right"});
}

TEST_CASE("An abandoned transaction leaves the document as it was", "[document][transaction]") {
    Document document;
    const ObjectId keep = document.addBody("Keep", makeBox(1.0, 1.0, 1.0));
    const ObjectId doomed = document.addBody("Doomed", makeBox(2.0, 2.0, 2.0));
    document.markSaved();

    SECTION("when it goes out of scope without a commit") {
        Transaction transaction(document, "Abandoned");
        document.addBody("Extra", makeBox(1.0, 1.0, 1.0));
        document.renameBody(keep, "Changed");
        document.setBodyVisible(keep, false);
        document.removeBody(doomed);
    }

    SECTION("when an exception passes through it") {
        try {
            Transaction transaction(document, "Failing");
            document.removeBody(doomed);
            document.addBody("Extra", makeBox(1.0, 1.0, 1.0));
            // The operation fails halfway: this shape is refused.
            document.addBody("Broken", lcad::kernel::Shape());
            transaction.commit();
            FAIL("the invalid body was accepted");
        } catch (const std::invalid_argument&) {
        }
    }

    CHECK(names(document) == std::vector<std::string>{"Keep", "Doomed"});
    CHECK(document.findBody(keep)->visible);
    CHECK_THAT(lcad::kernel::volume(document.findBody(doomed)->shape),
               WithinRel(8.0, exactPropertyTolerance));
    CHECK_FALSE(document.isModified());
    CHECK(document.undoLabel() == "Add Body");
    CHECK_FALSE(document.canRedo());
}

TEST_CASE("Transactions do not nest, and an empty one records nothing", "[document][transaction]") {
    Document document;

    {
        Transaction outer(document, "Outer");
        CHECK_THROWS_AS(Transaction(document, "Inner"), std::logic_error);
        outer.commit();
        CHECK_THROWS_AS(outer.commit(), std::logic_error);
    }

    CHECK_FALSE(document.canUndo());
    CHECK_FALSE(document.isModified());
    // And a new one can be opened afterwards.
    Transaction next(document, "Next");
    document.addBody("A", makeBox(1.0, 1.0, 1.0));
    next.commit();
    CHECK(document.undoLabel() == "Next");
}

TEST_CASE("The document knows whether it differs from what was saved", "[document][modified]") {
    Document document;

    document.addBody("A", makeBox(1.0, 1.0, 1.0));
    CHECK(document.isModified());

    document.markSaved();
    CHECK_FALSE(document.isModified());

    document.addBody("B", makeBox(1.0, 1.0, 1.0));
    CHECK(document.isModified());

    document.undo();
    CHECK_FALSE(document.isModified()); // back at the saved state

    document.undo();
    CHECK(document.isModified()); // before the saved state

    document.redo();
    CHECK_FALSE(document.isModified());

    SECTION("a different edit from the saved state's past can never be the saved state") {
        document.undo();
        document.addBody("C", makeBox(1.0, 1.0, 1.0));
        CHECK(document.isModified());
        document.undo();
        CHECK(document.isModified());
    }
}

TEST_CASE("Each committed transaction is a state of its own for the saved marker",
          "[document][modified][transaction]") {
    Document document;
    {
        Transaction first(document, "First");
        document.addBody("A", makeBox(1.0, 1.0, 1.0));
        first.commit();
    }
    document.markSaved();

    {
        Transaction second(document, "Second");
        document.addBody("B", makeBox(1.0, 1.0, 1.0));
        second.commit();
    }

    CHECK(document.isModified());
    document.undo();
    CHECK_FALSE(document.isModified());
}

TEST_CASE("Listeners hear about every change, in order", "[document][listener]") {
    Document document;
    std::vector<std::pair<Change::Kind, ObjectId>> heard;
    const auto id = document.addListener(
        [&](const Change& change) { heard.emplace_back(change.kind, change.body); });

    const ObjectId box = document.addBody("Box", makeBox(1.0, 1.0, 1.0));
    document.renameBody(box, "Crate");
    document.setBodyVisible(box, false);
    document.undo();
    document.removeBody(box);

    using Kind = Change::Kind;
    const std::vector<std::pair<Kind, ObjectId>> expected{
        {Kind::BodyAdded, box},
        {Kind::HistoryChanged, {}},
        {Kind::BodyRenamed, box},
        {Kind::HistoryChanged, {}},
        {Kind::BodyVisibilityChanged, box},
        {Kind::HistoryChanged, {}},
        {Kind::BodyVisibilityChanged, box},
        {Kind::HistoryChanged, {}}, // the undo
        {Kind::BodyRemoved, box},
        {Kind::HistoryChanged, {}},
    };
    CHECK(heard == expected);

    document.removeListener(id);
    document.undo();
    CHECK(heard.size() == expected.size());
}

TEST_CASE("A listener sees the document already changed, and may remove itself",
          "[document][listener]") {
    Document document;
    std::size_t bodiesSeen = 0;
    int calls = 0;
    Document::ListenerId id = 0;
    id = document.addListener([&](const Change& change) {
        ++calls;
        if (change.kind == Change::Kind::BodyAdded) {
            bodiesSeen = document.bodies().size();
            REQUIRE(document.findBody(change.body) != nullptr);
        }
        document.removeListener(id);
    });

    document.addBody("Box", makeBox(1.0, 1.0, 1.0));

    CHECK(bodiesSeen == 1);
    CHECK(calls == 1);
}

TEST_CASE("uniqueBodyName numbers a name that is taken", "[document]") {
    Document document;

    CHECK(document.uniqueBodyName("Box") == "Box");
    document.addBody("Box", makeBox(1.0, 1.0, 1.0));
    CHECK(document.uniqueBodyName("Box") == "Box 2");
    document.addBody("Box 2", makeBox(1.0, 1.0, 1.0));
    document.addBody("Box 4", makeBox(1.0, 1.0, 1.0));
    CHECK(document.uniqueBodyName("Box") == "Box 3");
    CHECK(document.uniqueBodyName("Sphere") == "Sphere");
}

TEST_CASE("A document rebuilt from saved state is clean, and bad state is refused", "[document]") {
    SECTION("consistent state") {
        DocumentState state;
        state.bodies.push_back(Body{ObjectId{2}, "Box", false, makeBox(1.0, 2.0, 3.0)});
        state.bodies.push_back(Body{ObjectId{5}, "Ball", true, makeSphere(1.0)});
        state.nextId = 9;

        Document document(std::move(state));

        CHECK(names(document) == std::vector<std::string>{"Box", "Ball"});
        CHECK_FALSE(document.findBody(ObjectId{2})->visible);
        CHECK_FALSE(document.isModified());
        CHECK_FALSE(document.canUndo());
        // New objects continue after the highest id the file ever used.
        CHECK(document.addBody("New", makeBox(1.0, 1.0, 1.0)) == ObjectId{9});
    }

    SECTION("two bodies with one id") {
        DocumentState state;
        state.bodies.push_back(Body{ObjectId{1}, "A", true, makeBox(1.0, 1.0, 1.0)});
        state.bodies.push_back(Body{ObjectId{1}, "B", true, makeBox(1.0, 1.0, 1.0)});
        state.nextId = 2;
        CHECK_THROWS_AS(Document(std::move(state)), std::invalid_argument);
    }

    SECTION("an id the document has not handed out yet") {
        DocumentState state;
        state.bodies.push_back(Body{ObjectId{7}, "A", true, makeBox(1.0, 1.0, 1.0)});
        state.nextId = 7;
        CHECK_THROWS_AS(Document(std::move(state)), std::invalid_argument);
    }

    SECTION("the id that means no object") {
        DocumentState state;
        state.bodies.push_back(Body{ObjectId{0}, "A", true, makeBox(1.0, 1.0, 1.0)});
        CHECK_THROWS_AS(Document(std::move(state)), std::invalid_argument);
    }

    SECTION("a body without a name or without geometry") {
        DocumentState unnamed;
        unnamed.bodies.push_back(Body{ObjectId{1}, "", true, makeBox(1.0, 1.0, 1.0)});
        unnamed.nextId = 2;
        CHECK_THROWS_AS(Document(std::move(unnamed)), std::invalid_argument);

        DocumentState empty;
        empty.bodies.push_back(Body{ObjectId{1}, "A", true, lcad::kernel::Shape()});
        empty.nextId = 2;
        CHECK_THROWS_AS(Document(std::move(empty)), std::invalid_argument);
    }
}
