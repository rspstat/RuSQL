"""Library of synthetic domain schemas used to generate NL->SQL training data.

Each Schema is a small, realistic multi-table database (RuSQL-compatible DDL). Column
`kind` drives what the query generator is allowed to do with it (equality filter,
range filter, aggregate, group-by, sort). `samples` gives a pool of realistic literal
values for text/enum columns so generated queries use plausible data, not placeholders.

HELD_OUT_DOMAINS lists schemas reserved for the test split only (never used to build
training examples), so evaluation can measure generalization to unseen schemas.
"""

from dataclasses import dataclass, field


@dataclass
class Column:
    name: str
    sql_type: str
    kind: str  # "pk" | "fk" | "text" | "number" | "date" | "bool" | "enum"
    primary_key: bool = False
    fk: tuple[str, str] | None = None  # (ref_table, ref_column)
    enum_values: list[str] = field(default_factory=list)
    samples: list[str] = field(default_factory=list)  # realistic literal values


@dataclass
class Table:
    name: str
    columns: list[Column]

    def col(self, name: str) -> Column:
        for c in self.columns:
            if c.name == name:
                return c
        raise KeyError(name)


@dataclass
class Schema:
    domain: str
    tables: list[Table]

    def table(self, name: str) -> Table:
        for t in self.tables:
            if t.name == name:
                return t
        raise KeyError(name)

    def create_table_sql(self) -> str:
        stmts = []
        for t in self.tables:
            col_defs = []
            for c in t.columns:
                parts = [c.name, c.sql_type]
                if c.primary_key:
                    parts.append("PRIMARY KEY")
                if c.fk:
                    parts.append(f"REFERENCES {c.fk[0]}({c.fk[1]})")
                col_defs.append(" ".join(parts))
            stmts.append(f"CREATE TABLE {t.name} (\n  " + ",\n  ".join(col_defs) + "\n);")
        return "\n".join(stmts)


def pk(name="id") -> Column:
    return Column(name, "INT", "pk", primary_key=True)


def fk(name, ref_table, ref_col="id") -> Column:
    return Column(name, "INT", "fk", fk=(ref_table, ref_col))


def text(name, length=50, samples: list[str] | None = None) -> Column:
    return Column(name, f"VARCHAR({length})", "text", samples=samples or [])


def number(name, sql_type="INT", samples: list[str] | None = None) -> Column:
    return Column(name, sql_type, "number", samples=samples or [])


def date_col(name) -> Column:
    return Column(name, "DATE", "date")


def boolean(name) -> Column:
    return Column(name, "BOOLEAN", "bool")


def enum_col(name, values: list[str]) -> Column:
    return Column(name, "VARCHAR(20)", "enum", enum_values=values)


# ---------------------------------------------------------------------------
# Schema library
# ---------------------------------------------------------------------------

SCHEMAS: dict[str, Schema] = {}


def _add(schema: Schema) -> None:
    SCHEMAS[schema.domain] = schema


_add(Schema("ecommerce", [
    Table("customer", [pk(), text("name", samples=["Alice Kim", "Bob Lee", "Carol Park", "David Choi"]),
                        text("email"), text("city", samples=["Seoul", "Busan", "Incheon", "Daegu"]),
                        date_col("created_at")]),
    Table("product", [pk(), text("name", samples=["Wireless Mouse", "Mechanical Keyboard", "USB-C Hub", "Monitor Stand"]),
                       enum_col("category", ["electronics", "accessories", "furniture", "office"]),
                       number("unit_price", "DECIMAL(10,2)"), number("stock_quantity")]),
    Table("orders", [pk(), fk("customer_id", "customer"), date_col("order_date"),
                     enum_col("status", ["pending", "shipped", "delivered", "cancelled"]),
                     number("total_amount", "DECIMAL(10,2)")]),
    Table("order_item", [pk(), fk("order_id", "orders"), fk("product_id", "product"), number("quantity")]),
]))

_add(Schema("company", [
    Table("department", [pk(), text("name", samples=["Engineering", "Sales", "Marketing", "HR", "Finance"]),
                          number("budget", "DECIMAL(12,2)")]),
    Table("employee", [pk(), text("name", samples=["Jane Smith", "Min-jun Kim", "So-yeon Lee", "Tom Baker"]),
                        fk("department_id", "department"), number("salary", "DECIMAL(10,2)"),
                        date_col("hire_date"), enum_col("status", ["active", "on_leave", "terminated"])]),
    Table("project", [pk(), text("name", samples=["Apollo", "Nova", "Zephyr", "Atlas"]),
                       fk("department_id", "department"), number("budget", "DECIMAL(12,2)"),
                       date_col("start_date"), date_col("end_date")]),
]))

_add(Schema("library", [
    Table("author", [pk(), text("name", samples=["J.K. Rowling", "Han Kang", "George Orwell", "Kim Young-ha"]),
                      text("country", samples=["UK", "South Korea", "USA", "Japan"])]),
    Table("book", [pk(), text("title", samples=["The Vegetarian", "1984", "Please Look After Mom", "Dune"]),
                   fk("author_id", "author"), enum_col("genre", ["fiction", "nonfiction", "poetry", "science"]),
                   number("published_year")]),
    Table("member", [pk(), text("name", samples=["Grace Han", "Peter Song", "Nina Cho"]), date_col("joined_date")]),
    Table("loan", [pk(), fk("book_id", "book"), fk("member_id", "member"), date_col("borrow_date"),
                   date_col("due_date"), date_col("return_date")]),
]))

_add(Schema("school", [
    Table("teacher", [pk(), text("name", samples=["Mr. Anderson", "Ms. Yoon", "Dr. Park"]),
                       text("subject", samples=["Math", "Science", "English", "History"])]),
    Table("student", [pk(), text("name", samples=["Sam", "Ji-ho", "Emma", "Yuna"]), number("grade"),
                       date_col("enrollment_date")]),
    Table("course", [pk(), text("name", samples=["Algebra I", "Biology", "World History", "Chemistry"]),
                      fk("teacher_id", "teacher"), number("credit")]),
    Table("enrollment", [pk(), fk("student_id", "student"), fk("course_id", "course"), number("score")]),
]))

_add(Schema("hospital", [
    Table("doctor", [pk(), text("name", samples=["Dr. Lee", "Dr. Kim", "Dr. Patel"]),
                      text("specialty", samples=["Cardiology", "Neurology", "Pediatrics", "Orthopedics"])]),
    Table("patient", [pk(), text("name", samples=["Min-ho", "Sarah", "Ji-eun", "Robert"]), number("age"),
                       text("gender", samples=["M", "F"])]),
    Table("appointment", [pk(), fk("patient_id", "patient"), fk("doctor_id", "doctor"),
                           date_col("appointment_date"), enum_col("status", ["scheduled", "completed", "cancelled"])]),
    Table("prescription", [pk(), fk("appointment_id", "appointment"),
                            text("medication", samples=["Amoxicillin", "Ibuprofen", "Metformin"]), number("dosage")]),
]))

_add(Schema("bank", [
    Table("branch", [pk(), text("name", samples=["Gangnam Branch", "Downtown Branch", "Airport Branch"]),
                      text("city", samples=["Seoul", "Busan", "Incheon"])]),
    Table("customer", [pk(), text("name", samples=["Alex Yoon", "Mia Chen", "Jake Oh"]), text("email")]),
    Table("account", [pk(), fk("customer_id", "customer"), fk("branch_id", "branch"),
                       enum_col("account_type", ["checking", "savings", "business"]),
                       number("balance", "DECIMAL(12,2)")]),
    Table("transaction", [pk(), fk("account_id", "account"),
                           enum_col("transaction_type", ["deposit", "withdrawal", "transfer"]),
                           number("amount", "DECIMAL(10,2)"), date_col("transaction_date")]),
]))

_add(Schema("restaurant", [
    Table("menu_item", [pk(), text("name", samples=["Bulgogi", "Bibimbap", "Kimchi Stew", "Fried Rice"]),
                         enum_col("category", ["appetizer", "main", "dessert", "beverage"]),
                         number("price", "DECIMAL(8,2)")]),
    Table("restaurant_table", [pk(), number("party_size"), enum_col("location", ["indoor", "outdoor", "bar"])]),
    Table("reservation", [pk(), fk("table_id", "restaurant_table"), text("customer_name", samples=["Ha-eun", "Marcus", "Yuki"]),
                           date_col("reservation_date"), number("party_size")]),
    Table("order_ticket", [pk(), fk("table_id", "restaurant_table"), fk("menu_item_id", "menu_item"),
                            number("quantity"), date_col("order_date")]),
]))

_add(Schema("hotel", [
    Table("room", [pk(), number("floor"), enum_col("room_type", ["single", "double", "suite"]),
                   number("price_per_night", "DECIMAL(8,2)")]),
    Table("guest", [pk(), text("name", samples=["Olivia Park", "Ethan Kim", "Sofia Reyes"]), text("email")]),
    Table("reservation", [pk(), fk("room_id", "room"), fk("guest_id", "guest"),
                           date_col("checkin_date"), date_col("checkout_date"),
                           enum_col("status", ["confirmed", "checked_in", "checked_out", "cancelled"])]),
]))

_add(Schema("gym", [
    Table("trainer", [pk(), text("name", samples=["Coach Rivera", "Coach Han", "Coach Lee"]),
                       text("specialty", samples=["Strength", "Cardio", "Yoga", "CrossFit"])]),
    Table("member", [pk(), text("name", samples=["Do-yun", "Hana", "Chris"]), date_col("start_date"),
                       enum_col("membership_type", ["basic", "premium", "vip"])]),
    Table("session", [pk(), fk("member_id", "member"), fk("trainer_id", "trainer"), date_col("session_date"),
                       number("duration_minutes")]),
]))

_add(Schema("blog", [
    Table("author", [pk(), text("name", samples=["writer_kim", "jane_doe", "min_writes"])]),
    Table("post", [pk(), fk("author_id", "author"), text("title", samples=["My Trip to Jeju", "Learning SQL", "Cooking Basics"]),
                    date_col("published_at"), number("view_count"), number("like_count")]),
    Table("comment", [pk(), fk("post_id", "post"), text("commenter_name", samples=["reader1", "guest_92", "book_lover"]),
                       date_col("created_at")]),
    Table("tag", [pk(), text("name", samples=["travel", "tech", "food", "life"])]),
]))

_add(Schema("movie_theater", [
    Table("movie", [pk(), text("title", samples=["Parasite", "The Matrix", "Oldboy", "Inception"]),
                     enum_col("genre", ["action", "drama", "comedy", "thriller"]), number("duration_minutes"),
                     date_col("release_date")]),
    Table("theater_room", [pk(), number("capacity"), enum_col("screen_type", ["standard", "imax", "3d"])]),
    Table("showtime", [pk(), fk("movie_id", "movie"), fk("room_id", "theater_room"), date_col("show_date")]),
    Table("ticket", [pk(), fk("showtime_id", "showtime"), text("customer_name", samples=["Ji-woo", "Aiden", "Sana"]),
                      number("price", "DECIMAL(8,2)")]),
]))

_add(Schema("car_rental", [
    Table("vehicle", [pk(), text("model", samples=["Sonata", "Tucson", "Model 3", "Civic"]),
                       enum_col("category", ["sedan", "suv", "compact", "luxury"]), number("mileage")]),
    Table("driver", [pk(), text("name", samples=["Noah Park", "Isabella Kim", "Liam Choi"]), text("license_number")]),
    Table("rental", [pk(), fk("vehicle_id", "vehicle"), fk("driver_id", "driver"),
                      date_col("rental_date"), date_col("return_date"), number("total_cost", "DECIMAL(10,2)")]),
]))

_add(Schema("warehouse", [
    Table("supplier", [pk(), text("name", samples=["Global Parts Co", "Fast Supply Inc", "Pacific Traders"])]),
    Table("product", [pk(), text("name", samples=["Steel Bolt", "Rubber Gasket", "Circuit Board", "Packaging Box"]),
                       fk("supplier_id", "supplier"), number("unit_cost", "DECIMAL(10,2)")]),
    Table("warehouse_location", [pk(), text("city", samples=["Ulsan", "Incheon", "Gwangju"]), number("capacity")]),
    Table("stock_movement", [pk(), fk("product_id", "product"), fk("warehouse_location_id", "warehouse_location"),
                              enum_col("movement_type", ["in", "out"]), number("quantity"), date_col("movement_date")]),
]))

_add(Schema("university_research", [
    Table("professor", [pk(), text("name", samples=["Prof. Song", "Prof. Nakamura", "Prof. Alvarez"]),
                         text("department", samples=["Computer Science", "Physics", "Biology"])]),
    Table("grad_student", [pk(), text("name", samples=["Do-hyun", "Wei", "Priya"]), fk("advisor_id", "professor")]),
    Table("publication", [pk(), fk("professor_id", "professor"), text("title", samples=["Deep Learning Advances", "Quantum Computing Survey"]),
                           number("year"), number("citation_count")]),
]))

_add(Schema("coffee_shop", [
    Table("barista", [pk(), text("name", samples=["Jenna", "Min-seo", "Leo"]),
                       text("specialty", samples=["Latte Art", "Cold Brew", "Espresso"])]),
    Table("customer", [pk(), text("name", samples=["Grace Moon", "Tom Reilly", "Yuna Baek"])]),
    Table("menu_item", [pk(), text("name", samples=["Americano", "Latte", "Croissant", "Cold Brew"]),
                         enum_col("category", ["coffee", "tea", "pastry", "snack"]),
                         number("price", "DECIMAL(8,2)")]),
    Table("cafe_order", [pk(), fk("customer_id", "customer"), fk("barista_id", "barista"),
                          date_col("order_date"), number("total_amount", "DECIMAL(8,2)")]),
]))

_add(Schema("veterinary_clinic", [
    Table("vet", [pk(), text("name", samples=["Dr. Suh", "Dr. Alvarez", "Dr. Mensah"]),
                  text("specialty", samples=["Surgery", "Dentistry", "Dermatology"])]),
    Table("pet_owner", [pk(), text("name", samples=["Ha-jun", "Melissa Cruz", "Owen Faulkner"]), text("phone")]),
    Table("pet", [pk(), text("name", samples=["Coco", "Max", "Nabi"]),
                  enum_col("species", ["dog", "cat", "rabbit", "bird"]), fk("owner_id", "pet_owner")]),
    Table("visit", [pk(), fk("pet_id", "pet"), fk("vet_id", "vet"), date_col("visit_date"),
                    text("diagnosis", samples=["Ear Infection", "Routine Checkup", "Vaccination"])]),
]))

_add(Schema("conference", [
    Table("speaker", [pk(), text("name", samples=["Dr. Adeyemi", "Rina Park", "Marcus Beltran"]),
                       text("topic", samples=["Distributed Systems", "Applied ML", "Cloud Security"])]),
    Table("attendee", [pk(), text("name", samples=["Seo-yeon", "Patrick Dunne", "Amara Okafor"]), text("email")]),
    Table("talk", [pk(), fk("speaker_id", "speaker"), text("title", samples=["Scaling Databases", "Prompting at Scale", "Zero Trust in Practice"]),
                   enum_col("track", ["ai", "security", "web", "data"]), date_col("talk_date")]),
    Table("registration", [pk(), fk("attendee_id", "attendee"), fk("talk_id", "talk"), date_col("registered_at")]),
]))

# --- Held-out domains (test split only, never used in training generation) ---

_add(Schema("real_estate", [
    Table("agent", [pk(), text("name", samples=["Emily Brooks", "Hyun-woo", "Carlos Diaz"])]),
    Table("client", [pk(), text("name", samples=["Grace Lim", "Daniel Wood"]), text("phone")]),
    Table("property", [pk(), fk("agent_id", "agent"), text("address", samples=["123 Maple St", "45 River Rd"]),
                        enum_col("property_type", ["apartment", "house", "condo"]), number("price", "DECIMAL(12,2)")]),
    Table("listing", [pk(), fk("property_id", "property"), fk("client_id", "client"), date_col("listed_date"),
                       enum_col("status", ["active", "pending", "sold"])]),
]))

_add(Schema("airline", [
    Table("airport", [pk(), text("name", samples=["Incheon Intl", "JFK", "Narita"]), text("city", samples=["Seoul", "New York", "Tokyo"])]),
    Table("flight", [pk(), fk("origin_id", "airport"), fk("destination_id", "airport"),
                      date_col("departure_date"), number("price", "DECIMAL(10,2)")]),
    Table("passenger", [pk(), text("name", samples=["Ji-min", "Oscar", "Lena"])]),
    Table("booking", [pk(), fk("flight_id", "flight"), fk("passenger_id", "passenger"),
                       enum_col("seat_class", ["economy", "business", "first"]), date_col("booking_date")]),
]))

_add(Schema("insurance", [
    Table("agent", [pk(), text("name", samples=["Robert Kang", "Susan White"])]),
    Table("customer", [pk(), text("name", samples=["Tae-yang", "Maria Lopez"]), number("age")]),
    Table("policy", [pk(), fk("customer_id", "customer"), fk("agent_id", "agent"),
                      enum_col("policy_type", ["auto", "home", "health", "life"]), number("premium", "DECIMAL(10,2)")]),
    Table("claim", [pk(), fk("policy_id", "policy"), date_col("claim_date"), number("claim_amount", "DECIMAL(10,2)"),
                     enum_col("status", ["submitted", "approved", "rejected"])]),
]))

HELD_OUT_DOMAINS = {"real_estate", "airline", "insurance"}
TRAIN_DOMAINS = [d for d in SCHEMAS if d not in HELD_OUT_DOMAINS]
