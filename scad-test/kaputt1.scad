projection(cut=true)
translate([0,0,-10]) scale(20) polyhedron(
    points=[
        [0,0,0],
        [0,0,1],
        [0,1,0],
        [0,1,1],
        [1,0,0],
        [1,0,1],
        [1,1,0],
        [1,1,1]
    ],
    faces=[
        [1,0,2,3],
        [0,1,5,4],
        // [3,2,4,5],
        // [0,4,2],
        // [1,3,5],
    ]
);
